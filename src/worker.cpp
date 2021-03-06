#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <netinet/in.h>
#include <Poco/Stopwatch.h>
#include <Poco/URI.h>
#include <Poco/Net/IPAddress.h>
#include <sstream>
#include <iomanip>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_cycles.h>
#include <rte_ip_frag.h>
#include <memory>

#include "worker.h"
#include "main.h"
#include "sendertask.h"
#include "flow.h"
#include "distributor.h"
#include <rte_hash.h>

#define tcphdr(x)	((struct tcphdr *)(x))

//#define DEBUG_TIME


WorkerThread::WorkerThread(const std::string& name, WorkerConfig &workerConfig, flowHash *fh, Distributor *distr, int worker_id) :
		m_WorkerConfig(workerConfig), m_Stop(true),
		_logger(Poco::Logger::get(name)),
		 m_FlowHash(fh),
		_distr(distr),
		_worker_id(worker_id)
{
	ipv4_flows = (struct ndpi_flow_info **)calloc(fh->getHashSize(),sizeof(struct ndpi_flow_info *));
	if(ipv4_flows == nullptr)
	{
		_logger.fatal("Not enough memory for ipv4 flows");
		throw Poco::Exception("Not enough memory for ipv4 flows");
	}
	ipv6_flows = (struct ndpi_flow_info **)calloc(fh->getHashSize(),sizeof(struct ndpi_flow_info *));
	if(ipv6_flows == nullptr)
	{
		_logger.fatal("Not enough memory for ipv6 flows");
		throw Poco::Exception("Not enough memory for ipv6 flows");
	}
	_logger.debug("Allocating %d bytes for flow pool", (int) (fh->getHashSize()*2*sizeof(struct ndpi_flow_info)));
	std::string mempool_name("flows_pool_" + name);
	flows_pool = rte_mempool_create(mempool_name.c_str(), fh->getHashSize()*2, sizeof(struct ndpi_flow_info), 0, 0, NULL, NULL, NULL, NULL, rte_socket_id(), 0);
	if(flows_pool == nullptr)
	{
		_logger.fatal("Not enough memory for flows pool. Tried to allocate %d bytes", (int) (fh->getHashSize()*2*sizeof(struct ndpi_flow_info)));
		throw Poco::Exception("Not enough memory for flows pool");
	}
	uri.reserve(URI_RESERVATION_SIZE);
}

WorkerThread::~WorkerThread()
{
	free(ipv4_flows);
	free(ipv6_flows);
	rte_mempool_free(flows_pool);
}

ndpi_flow_info *WorkerThread::getFlow(uint8_t *ip_header, int ip_version, uint64_t timestamp)
{
	if(ip_version == 6)
	{
		struct ipv6_5tuple key;
		struct ipv6_hdr *ipv6_header = (struct ipv6_hdr *) ip_header;
		m_FlowHash->makeIPv6Key(ipv6_header,&key);
		int32_t ret = rte_hash_lookup(m_FlowHash->getIPv6Hash(), &key);
		if(ret >= 0)
		{
			return ipv6_flows[ret];
		}
		if(ret == -EINVAL)
		{
			_logger.error("Bad parameter in ipv6 hash lookup");
			return NULL;
		}
		if(ret == -ENOENT)
		{
			struct ndpi_flow_info *newflow;
			if(rte_mempool_get(flows_pool, (void **)&newflow) != 0)
			{
				_logger.fatal("Not enough memory for the flow in the flows_pool");
				return NULL;
			}
			memset(newflow,0,sizeof(struct ndpi_flow_info));
			newflow->last_seen = timestamp;
			newflow->ip_version = 6;
			newflow->cli2srv_direction = true;
			newflow->block = false;
			memcpy(&newflow->keys.ipv6_key, &key, sizeof(struct ipv6_5tuple));
			newflow->src_id = (struct ndpi_id_struct*)calloc(1, SIZEOF_ID_STRUCT);
			newflow->dst_id = (struct ndpi_id_struct*)calloc(1, SIZEOF_ID_STRUCT);
			newflow->ndpi_flow = (struct ndpi_flow_struct *)calloc(1, SIZEOF_FLOW_STRUCT);
			if(newflow->src_id == NULL || newflow->dst_id == NULL || newflow->ndpi_flow == NULL)
			{
				_logger.fatal("Not enough memory for the flow");
				return NULL;
			}
			ret = rte_hash_add_key(m_FlowHash->getIPv6Hash(), &key);
			if(ret == -EINVAL)
			{
				free(newflow->src_id);
				free(newflow->dst_id);
				free(newflow->ndpi_flow);
				rte_mempool_put(flows_pool,newflow);
				_logger.fatal("Bad parameters in hash add");
				return NULL;
			}
			if(ret == -ENOSPC)
			{
				free(newflow->src_id);
				free(newflow->dst_id);
				free(newflow->ndpi_flow);
				rte_mempool_put(flows_pool,newflow);
				_logger.fatal("There is no space in the ipv6 flow hash");
				return NULL;
			}
			ipv6_flows[ret] = newflow;
			m_ThreadStats.ndpi_ipv6_flows_count++;
			m_ThreadStats.ndpi_flows_count++;
			return newflow;
		}
		return NULL;
	}
	if(ip_version == 4)
	{
		struct ipv4_5tuple key;
		struct ipv4_hdr *ipv4_header = (struct ipv4_hdr *) ip_header;
		m_FlowHash->makeIPv4Key(ipv4_header,&key);
		int32_t ret = rte_hash_lookup(m_FlowHash->getIPv4Hash(), &key);
		if(ret >= 0)
		{
			return ipv4_flows[ret];
		}
		if(ret == -EINVAL)
		{
			_logger.error("Bad parameter in ipv4 hash lookup");
			return NULL;
		}
		if(ret == -ENOENT)
		{
			struct ndpi_flow_info *newflow;
			if(rte_mempool_get(flows_pool, (void **)&newflow) != 0)
			{
				_logger.fatal("Not enough memory for the flow in the flows_pool");
				return NULL;
			}
			memset(newflow,0,sizeof(struct ndpi_flow_info));
			newflow->last_seen = timestamp;
			newflow->ip_version = 4;
			newflow->cli2srv_direction = true;
			newflow->block = false;
			memcpy(&newflow->keys.ipv4_key, &key, sizeof(struct ipv4_5tuple));
			newflow->src_id = (struct ndpi_id_struct*)calloc(1, SIZEOF_ID_STRUCT);
			newflow->dst_id = (struct ndpi_id_struct*)calloc(1, SIZEOF_ID_STRUCT);
			newflow->ndpi_flow = (struct ndpi_flow_struct *)calloc(1, SIZEOF_FLOW_STRUCT);
			if(newflow->src_id == NULL || newflow->dst_id == NULL || newflow->ndpi_flow == NULL)
			{
				_logger.fatal("Not enough memory for the flow");
				return NULL;
			}
			ret = rte_hash_add_key(m_FlowHash->getIPv4Hash(), &key);
			if(ret == -EINVAL)
			{
				free(newflow->src_id);
				free(newflow->dst_id);
				free(newflow->ndpi_flow);
				rte_mempool_put(flows_pool,newflow);
				_logger.fatal("Bad parameters in hash add");
				return NULL;
			}
			if(ret == -ENOSPC)
			{
				free(newflow->src_id);
				free(newflow->dst_id);
				free(newflow->ndpi_flow);
				rte_mempool_put(flows_pool,newflow);
				_logger.fatal("There is no space in the ipv4 flow hash");
				return NULL;
			}
			ipv4_flows[ret] = newflow;
			m_ThreadStats.ndpi_ipv4_flows_count++;
			m_ThreadStats.ndpi_flows_count++;
			return newflow;
		}
		return NULL;
	}
	return NULL;
}



bool WorkerThread::analyzePacket(struct rte_mbuf* m, uint64_t timestamp)
{
	struct ether_hdr *eth_hdr;
	uint16_t ether_type;
	uint8_t *l3;
	uint16_t l4_packet_len;
	uint16_t payload_len;
	struct ipv4_hdr *ipv4_header;
	struct ipv6_hdr *ipv6_header;
	int size=rte_pktmbuf_pkt_len(m);

	eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);
	l3 = (uint8_t *)eth_hdr + sizeof(struct ether_hdr);

	int ip_version=0;
	uint32_t ip_len;
	int iphlen=0;

	if(ether_type == ETHER_TYPE_VLAN || ether_type == 0x8847)
	{
		while(1)
		{
			if(ether_type == ETHER_TYPE_VLAN)
			{
				struct vlan_hdr *vlan_hdr = (struct vlan_hdr *)(l3);
				ether_type = rte_be_to_cpu_16(vlan_hdr->eth_proto);
				l3 += sizeof(struct vlan_hdr);
			} else if(ether_type == 0x8847)
			{
				uint8_t bos;
				bos = ((uint8_t *)eth_hdr)[2] & 0x1;
				eth_hdr = rte_pktmbuf_mtod_offset(m, struct ether_hdr *,4);
				if(bos)
				{
					ether_type = ETHER_TYPE_IPv4;
					break;
				}
			} else
				break;
		}
	}

	// определяем версию протокола
	if (ether_type == ETHER_TYPE_IPv4)
	{
		ip_version = 4;
		m_ThreadStats.ipv4_packets++;
		ipv4_header = (struct ipv4_hdr *)l3;
		ip_len=rte_be_to_cpu_16(ipv4_header->total_length);
		iphlen = (ipv4_header->version_ihl & IPV4_HDR_IHL_MASK) * IPV4_IHL_MULTIPLIER; // ipv4
		l4_packet_len = ip_len - iphlen;
		if(ip_len < 20)
		{
			m_ThreadStats.ipv4_short_packets++;
			return false;
		}
		if(rte_ipv4_frag_pkt_is_fragmented(ipv4_header))
		{
			m_ThreadStats.ipv4_fragments++;
			return false;
		}
	} else if (ether_type == ETHER_TYPE_IPv6)
	{
		ip_version=6;
		m_ThreadStats.ipv6_packets++;
		ipv6_header = (struct ipv6_hdr *)l3;
		ip_len=rte_be_to_cpu_16(ipv6_header->payload_len) + sizeof(ipv6_hdr);
		iphlen = sizeof(struct ipv6_hdr);
		l4_packet_len = rte_be_to_cpu_16(ipv6_header->payload_len);
		if(rte_ipv6_frag_get_ipv6_fragment_header(ipv6_header) != NULL)
		{
			m_ThreadStats.ipv6_fragments++;
			return false;
		}
	} else {
		//_logger.debug("Unsupported ethernet type %x", (int) ether_type);
		return false;
	}

	m_ThreadStats.ip_packets++;

	uint8_t ip_protocol=(ip_version == 4 ? ipv4_header->next_proto_id : ipv6_header->proto);

	if(ip_protocol != IPPROTO_TCP)
	{
		//_logger.debug("Not TCP protocol");
		return false;
	}

	m_ThreadStats.total_bytes += size;

	uint8_t *pkt_data_ptr = NULL;
	struct tcphdr* tcph;

	pkt_data_ptr = l3 + (ip_version == 4 ? sizeof(struct ipv4_hdr) : sizeof(struct ipv6_hdr));

	tcph = (struct tcphdr *) pkt_data_ptr;

	// длина tcp заголовка
	int tcphlen = tcphdr(l3+iphlen)->doff*4;

	// общая длина всех заголовков
	uint32_t hlen = iphlen + tcphlen;

	// пропускаем пакет без данных
	if(hlen == ip_len)
	{
		return false;
	}

	payload_len = l4_packet_len-tcphlen;

	m_ThreadStats.analyzed_packets++;

	int tcp_src_port=rte_be_to_cpu_16(tcph->source);
	int tcp_dst_port=rte_be_to_cpu_16(tcph->dest);

	std::unique_ptr<Poco::Net::IPAddress> src_ip;
	std::unique_ptr<Poco::Net::IPAddress> dst_ip;
	if(ip_version == 4)
	{
		src_ip.reset(new Poco::Net::IPAddress(&ipv4_header->src_addr,sizeof(in_addr)));
		dst_ip.reset(new Poco::Net::IPAddress(&ipv4_header->dst_addr,sizeof(in_addr)));
	} else {
		src_ip.reset(new Poco::Net::IPAddress(&ipv6_header->src_addr,sizeof(in6_addr)));
		dst_ip.reset(new Poco::Net::IPAddress(&ipv6_header->dst_addr,sizeof(in6_addr)));
	}


	if(m_WorkerConfig.ipportMap && m_WorkerConfig.ipportMapLock.tryLock())
	{
	
		if(m_WorkerConfig.ipPortMap->try_search_exact_ip(*dst_ip.get()))
		{

			IPPortMap::iterator it_ip=m_WorkerConfig.ipportMap->find(*dst_ip.get());
			if(it_ip != m_WorkerConfig.ipportMap->end())
			{
				unsigned short port=tcp_dst_port;
				if (it_ip->second.size() == 0 || it_ip->second.find(port) != it_ip->second.end())
				{
					m_WorkerConfig.ipportMapLock.unlock();
					m_ThreadStats.matched_ip_port++;
					_logger.debug("Found record in ip:port list for the client %s:%d and server %s:%d",src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
					std::string empty_str;
					SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq, 0, empty_str, true));
					m_ThreadStats.sended_rst++;
					return true;
				}
			}
		}
		m_WorkerConfig.ipportMapLock.unlock();
	}
	/* setting time */
	uint64_t packet_time = timestamp;

	ndpi_flow_info *flow_info = getFlow(l3, ip_version, timestamp);

	if(!flow_info)
	{
//		_logger.fatal("Flow info is null, can't proceed packet");
		return false;
	}


	flow_info->last_seen = timestamp;

	if(flow_info->detection_completed && flow_info->block == false)
		return false;

	if(flow_info->detection_completed && flow_info->block == true)
	{
		m_ThreadStats.already_detected_blocked++;
//		_logger.information("Got already blocked flow. Protocol %d. Src port in packet %d in flow %d, dst port in packet %d in flow %d. Source ip in packet %s in flow %d, dst ip in packet %s in flow %d", (int) flow_info->detected_protocol.protocol,(int)tcp_src_port,(int) flow_info->keys.ipv4_key.port_src,(int)tcp_dst_port,(int) flow_info->keys.ipv4_key.port_dst, src_ip->toString(), (int) flow_info->keys.ipv4_key.ip_src, dst_ip->toString(), (int) flow_info->keys.ipv4_key.ip_dst);
	}

	flow_info->detected_protocol = ndpi_detection_process_packet(m_WorkerConfig.ndpi_struct, flow_info->ndpi_flow,
		l3,
		ip_len,
		packet_time, (struct ndpi_id_struct *) flow_info->src_id, (struct ndpi_id_struct *) flow_info->dst_id);

	if(flow_info->detected_protocol.protocol == NDPI_PROTOCOL_UNKNOWN)
	{
		flow_info->detected_protocol = ndpi_detection_giveup(m_WorkerConfig.ndpi_struct, flow_info->ndpi_flow);
	}

	if(flow_info->detected_protocol.protocol == NDPI_PROTOCOL_UNKNOWN)
	{
		flow_info->detected_protocol = ndpi_guess_undetected_protocol(m_WorkerConfig.ndpi_struct,
			ip_protocol,
			0,//ip
			tcp_src_port, // sport
			0,
			tcp_dst_port); // dport
	}

	flow_info->bytes += ip_len;
	flow_info->packets++;
	if(flow_info->detected_protocol.protocol != NDPI_PROTOCOL_SSL && flow_info->detected_protocol.protocol != NDPI_PROTOCOL_SSL && flow_info->detected_protocol.protocol == NDPI_PROTOCOL_TOR && flow_info->detected_protocol.master_protocol != NDPI_PROTOCOL_HTTP && flow_info->detected_protocol.protocol != NDPI_PROTOCOL_HTTP && flow_info->detected_protocol.protocol != NDPI_PROTOCOL_DIRECT_DOWNLOAD_LINK)
		flow_info->detection_completed = true;

	if(flow_info->detected_protocol.master_protocol == NDPI_PROTOCOL_SSL || flow_info->detected_protocol.protocol == NDPI_PROTOCOL_SSL || flow_info->detected_protocol.protocol == NDPI_PROTOCOL_TOR)
	{
		if(m_WorkerConfig.atmSSLDomains && flow_info->ndpi_flow->l4.tcp.ssl_seen_client_cert == 1)
		{
			std::string ssl_client;
			if(flow_info->ndpi_flow->protos.ssl.client_certificate[0] != '\0')
			{
				ssl_client=flow_info->ndpi_flow->protos.ssl.client_certificate;
//				_logger.debug("SSL client is: %s",ssl_client);
			}
			if(!ssl_client.empty())
			{
				// если не можем выставить lock, то нет смысла продолжать...
				if(!m_WorkerConfig.atmSSLDomainsLock.tryLock())
					return false;
#ifdef DEBUG_TIME
				sw.reset();
				sw.start();
#endif
				if(m_WorkerConfig.lower_host)
					std::transform(ssl_client.begin(), ssl_client.end(), ssl_client.begin(), ::tolower);
				AhoCorasickPlus::Match match;
				std::size_t host_len=ssl_client.length();
				bool found=false;
				{
					m_WorkerConfig.atmSSLDomains->search(ssl_client,false);
					while(m_WorkerConfig.atmSSLDomains->findNext(match) && !found)
					{
						if(match.pattern.ptext.length != host_len)
						{
							DomainsMatchType::Iterator it=m_WorkerConfig.SSLdomainsMatchType->find(match.id);
							bool exact_match=false;
							if(it != m_WorkerConfig.SSLdomainsMatchType->end())
								exact_match = it->second;
							if(exact_match)
								continue;
							if(ssl_client[host_len-match.pattern.ptext.length-1] != '.')
								continue;
						}
						found=true;
					}
				}
				m_WorkerConfig.atmSSLDomainsLock.unlock();
#ifdef DEBUG_TIME
				sw.stop();
				_logger.debug("SSL Host seek occupied %ld us, host: %s",sw.elapsed(),ssl_client);
#endif
				if(found)
				{
					m_ThreadStats.matched_ssl++;
					_logger.debug("SSL host %s present in SSL domain (file line %u) list from ip %s:%d to ip %s:%d", ssl_client, match.id, src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
					std::string empty_str;
					SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq, 0, empty_str, true));
					m_ThreadStats.sended_rst++;
					flow_info->block=true;
					return true;
				} else {
					return false;
				}
			} else if(m_WorkerConfig.block_undetected_ssl)
			{
				if(m_WorkerConfig.sslIPsLock.tryLock())
				{
					if(m_WorkerConfig.sslIPs->try_search_exact_ip(*dst_ip.get()))
					{
						m_WorkerConfig.sslIPsLock.unlock();
						m_ThreadStats.matched_ssl_ip++;
						_logger.debug("Blocking/Marking SSL client hello packet from %s:%d to %s:%d", src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
						m_ThreadStats.sended_rst++;
						std::string empty_str;
						SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq, 0, empty_str, true));
						flow_info->block=true;
						return true;
					}
					m_WorkerConfig.sslIPsLock.unlock();
				}
			}
		}
		return false;
	}


	if(flow_info->detected_protocol.master_protocol != NDPI_PROTOCOL_HTTP && flow_info->detected_protocol.protocol != NDPI_PROTOCOL_HTTP && flow_info->detected_protocol.protocol != NDPI_PROTOCOL_DIRECT_DOWNLOAD_LINK)
	{
//		flow_info->detection_completed = true;
		return false;
	}

	if((flow_info->ndpi_flow->http.method == HTTP_METHOD_GET || flow_info->ndpi_flow->http.method == HTTP_METHOD_POST || flow_info->ndpi_flow->http.method == HTTP_METHOD_HEAD) && flow_info->ndpi_flow->http.url != NULL)
	{
		if(m_WorkerConfig.atm)
		{
			if(m_WorkerConfig.atmLock.tryLock())
			{
				if(m_WorkerConfig.url_normalization)
				{
					try
					{
						Poco::URI uri_p(flow_info->ndpi_flow->http.url);
						uri_p.normalize();
						uri.assign(uri_p.toString());
					} catch (Poco::SyntaxException &ex)
					{
						uri.assign(flow_info->ndpi_flow->http.url);
						_logger.debug("An SyntaxException occured: '%s' on URI: '%s'", ex.displayText(), uri);
					}
				} else {
					uri.assign(flow_info->ndpi_flow->http.url);
				}
				if(m_WorkerConfig.remove_dot || (!m_WorkerConfig.url_normalization && m_WorkerConfig.lower_host))
				{
					// remove dot after domain...
					size_t f_slash_pos=uri.find('/',10);
					if(!m_WorkerConfig.url_normalization && m_WorkerConfig.lower_host && f_slash_pos != std::string::npos)
					{
						std::transform(uri.begin()+7, uri.begin()+f_slash_pos, uri.begin()+7, ::tolower);
					}
					if(m_WorkerConfig.remove_dot && f_slash_pos != std::string::npos)
					{
						if(uri[f_slash_pos-1] == '.')
							uri.erase(f_slash_pos-1,1);
					}
				}
				AhoCorasickPlus::Match match;
				bool found=false;
				size_t uri_length=uri.length() - 7;
				char const *uri_ptr=uri.c_str() + 7;
				m_WorkerConfig.atm->search((char *)uri_ptr, uri_length, false); // skip http://
				EntriesData::Iterator it;
				while(m_WorkerConfig.atm->findNext(match) && !found)
				{
					it=m_WorkerConfig.entriesData->find(match.id);
					if(match.pattern.ptext.length != uri_length)
					{
						int r=match.position-match.pattern.ptext.length;
						if(it->second.type == E_TYPE_DOMAIN)
						{
							if(r > 0)
							{
								if(it->second.match_exactly)
									continue;
								if(*(uri_ptr+r-1) != '.')
									continue;
							}
						} else if(it->second.type == E_TYPE_URL)
						{
							if(m_WorkerConfig.match_url_exactly)
								continue;
							if(r > 0)
							{
								if(*(uri_ptr+r-1) != '.')
									continue;
							}
						}
					}
					found=true;
				}
				m_WorkerConfig.atmLock.unlock();
				if(found)
				{
					if(it->second.type == E_TYPE_DOMAIN) // block by domain...
					{
						m_ThreadStats.matched_domains++;
//						_logger.debug("Host %s present in domain (file line %u) list from ip %s to ip %s", host, match.id, src_ip->toString(), dst_ip->toString());
						if(m_WorkerConfig.http_redirect)
						{
							std::string add_param;
							switch (m_WorkerConfig.add_p_type)
							{
								case A_TYPE_ID: add_param="id="+std::to_string(it->second.lineno);
									break;
								case A_TYPE_URL: add_param="url="+uri;
									break;
								default: break;
							}
							SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port, src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ rte_cpu_to_be_32(rte_be_to_cpu_32(tcph->seq)+payload_len),/* flag psh */ 1, add_param));
							m_ThreadStats.redirected_domains++;
						} else {
							std::string empty_str;
							SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq, 0, empty_str, true));
							m_ThreadStats.sended_rst++;
						}
						return true;
					} else if(it->second.type == E_TYPE_URL) // block by url...
					{
						m_ThreadStats.matched_urls++;
//						_logger.debug("URL %s present in url (file pos %u) list from ip %s to ip %s", uri, match.id, src_ip->toString(), dst_ip->toString());
						if(m_WorkerConfig.http_redirect)
						{
							std::string add_param;
							switch (m_WorkerConfig.add_p_type)
							{
								case A_TYPE_ID: add_param="id="+std::to_string(it->second.lineno);
									break;
								case A_TYPE_URL: add_param="url="+uri;
									break;
								default: break;
							}
							SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port, src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ rte_cpu_to_be_32(rte_be_to_cpu_32(tcph->seq)+payload_len),/* flag psh */ 1, add_param));
							m_ThreadStats.redirected_urls++;
						} else {
							std::string empty_str;
							SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(), /*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq, 0, empty_str, true));
							m_ThreadStats.sended_rst++;
						}
						flow_info->block=true;
						return true;
					}

				}


			}
		}
	}
	return false;
}



bool WorkerThread::run(uint32_t coreId)
{
	setCoreId(coreId);
//	m_CoreId = coreId;
	m_Stop = false;
	struct rte_mbuf *buf;

	const uint64_t timeout = FLOW_IDLE_TIME * rte_get_timer_hz();

	const uint64_t gc_int_tsc = (extFilter::getTscHz() + US_PER_S - 1) / US_PER_S * EXTF_GC_INTERVAL;

	int32_t n_flows=m_FlowHash->getHashSize();
	int gc_budget = ((double)n_flows/(EXTF_ALL_GC_INTERVAL*1000*1000))*EXTF_GC_INTERVAL;

	_logger.debug("gc_budget = %d",gc_budget);

	uint64_t cur_tsc,diff_gc_tsc;
	uint64_t prev_gc_tsc=0;
	_logger.debug("Starting working thread on core %u", coreId);
	_logger.debug("Running gc clean every %" PRIu64 " cycles. Cycles per second %" PRIu64, gc_int_tsc, rte_get_timer_hz());

	int32_t iter_flows = 0;
	// main loop, runs until be told to stop
	while (!m_Stop)
	{
		rte_distributor_request_pkt(_distr->getDistributor(), _worker_id, NULL);
		while((buf = rte_distributor_poll_pkt(_distr->getDistributor(), _worker_id)) == NULL)
		{
			if(m_Stop)
				break;
			rte_pause();
		}
		if (unlikely(buf == NULL))
			continue;
		cur_tsc = rte_rdtsc();
		last_time = cur_tsc;

		rte_prefetch0(rte_pktmbuf_mtod(buf, void *));

		// count received packets
		m_ThreadStats.total_packets++;
		analyzePacket(buf, last_time);
		rte_pktmbuf_free(buf);

		diff_gc_tsc = cur_tsc - prev_gc_tsc;
		if (unlikely(diff_gc_tsc >= gc_int_tsc))
		{
			int z=0;
			while(z < gc_budget && iter_flows < n_flows)
			{
				if(ipv4_flows[iter_flows] && ((ipv4_flows[iter_flows]->last_seen+timeout) < cur_tsc))
				{
					int32_t delr=rte_hash_del_key(m_FlowHash->getIPv4Hash(), &ipv4_flows[iter_flows]->keys.ipv4_key);
					if(delr < 0)
					{
						_logger.error("Error (%d) occured while delete data from the ipv4 flow hash table", (int)delr);
					} else {
						ipv4_flows[iter_flows]->free_mem();
						rte_mempool_put(flows_pool,ipv4_flows[iter_flows]);
						ipv4_flows[iter_flows] = nullptr;
						m_ThreadStats.ndpi_flows_count--;
						m_ThreadStats.ndpi_ipv4_flows_count--;
						m_ThreadStats.ndpi_flows_deleted++;
					}
				}
				if(ipv6_flows[iter_flows] && ((ipv6_flows[iter_flows]->last_seen+timeout) < cur_tsc))
				{
					int32_t delr=rte_hash_del_key(m_FlowHash->getIPv6Hash(), &ipv6_flows[iter_flows]->keys.ipv6_key);
					if(delr < 0)
					{
						_logger.error("Error (%d) occured while delete data from the ipv6 flow hash table", (int)delr);
					} else {
						ipv6_flows[iter_flows]->free_mem();
						rte_mempool_put(flows_pool,ipv6_flows[iter_flows]);
						ipv6_flows[iter_flows] = nullptr;
						m_ThreadStats.ndpi_flows_count--;
						m_ThreadStats.ndpi_ipv6_flows_count--;
						m_ThreadStats.ndpi_flows_deleted++;
					}
				}
				z++;
				iter_flows++;
			}
			iter_flows &= (n_flows-1);
			prev_gc_tsc = cur_tsc;
		}
	}
	_logger.debug("Worker thread on core %u terminated", coreId);
	return true;
}
