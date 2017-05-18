//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "NetworkSocketWinsock.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP

#else
#include <IPHlpApi.h>
#endif
#include <assert.h>
#include "../../logging.h"
#include "../../VoIPController.h"

using namespace tgvoip;

NetworkSocketWinsock::NetworkSocketWinsock() :
	lastRecvdV4(0), lastRecvdV6("::0")
{
	needUpdateNat64Prefix = true;
	nat64Present = false;
	switchToV6at = 0;
	isV4Available = false;

#ifdef TGVOIP_WINXP_COMPAT
	DWORD version = GetVersion();
	isAtLeastVista = LOBYTE(LOWORD(version)) >=
	                 6; // Vista is 6.0, XP is 5.1 and 5.2
#else
	isAtLeastVista = true;
#endif

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	LOGD("Initialized winsock, version %d.%d",
	     wsaData.wHighVersion, wsaData.wVersion);
}

NetworkSocketWinsock::~NetworkSocketWinsock()
{

}

void NetworkSocketWinsock::SetMaxPriority()
{

}

void NetworkSocketWinsock::Send(NetworkPacket
                                *packet)
{
	sockaddr_in6 addr;
	IPv4Address *v4addr = dynamic_cast<IPv4Address *>
	                      (packet->address);
	if(v4addr) {
		if(!isAtLeastVista) {
			sockaddr_in _addr;
			_addr.sin_family = AF_INET;
			_addr.sin_addr.s_addr = v4addr->GetAddress();
			_addr.sin_port = htons(packet->port);
			int res = sendto(fd, (char *)packet->data,
			                 packet->length, 0, (sockaddr *)&_addr,
			                 sizeof(_addr));
			if(res == SOCKET_ERROR) {
				int error = WSAGetLastError();
				LOGE("error sending: %d", error);
			}
			return;
		}
		if(needUpdateNat64Prefix && !isV4Available &&
		        VoIPController::GetCurrentTime() > switchToV6at &&
		        switchToV6at != 0) {
			LOGV("Updating NAT64 prefix");
			nat64Present = false;
			addrinfo *addr0;
			int res = getaddrinfo("ipv4only.arpa", NULL, NULL,
			                      &addr0);
			if(res != 0) {
				LOGW("Error updating NAT64 prefix: %d / %s", res,
				     gai_strerror(res));
			} else {
				addrinfo *addrPtr;
				unsigned char *addr170 = NULL;
				unsigned char *addr171 = NULL;
				for(addrPtr = addr0; addrPtr;
				        addrPtr = addrPtr->ai_next) {
					if(addrPtr->ai_family == AF_INET6) {
						sockaddr_in6 *translatedAddr = (sockaddr_in6 *)
						                               addrPtr->ai_addr;
						uint32_t v4part = *((uint32_t *)
						                    &translatedAddr->sin6_addr.s6_addr[12]);
						if(v4part == 0xAA0000C0 && !addr170) {
							addr170 = translatedAddr->sin6_addr.s6_addr;
						}
						if(v4part == 0xAB0000C0 && !addr171) {
							addr171 = translatedAddr->sin6_addr.s6_addr;
						}
						char buf[INET6_ADDRSTRLEN];
						//LOGV("Got translated address: %s", inet_ntop(AF_INET6, &translatedAddr->sin6_addr, buf, sizeof(buf)));
					}
				}
				if(addr170 && addr171 &&
				        memcmp(addr170, addr171, 12) == 0) {
					nat64Present = true;
					memcpy(nat64Prefix, addr170, 12);
					char buf[INET6_ADDRSTRLEN];
					//LOGV("Found nat64 prefix from %s", inet_ntop(AF_INET6, addr170, buf, sizeof(buf)));
				} else {
					LOGV("Didn't find nat64");
				}
				freeaddrinfo(addr0);
			}
			needUpdateNat64Prefix = false;
		}
		memset(&addr, 0, sizeof(sockaddr_in6));
		addr.sin6_family = AF_INET6;
		*((uint32_t *)&addr.sin6_addr.s6_addr[12]) =
		    v4addr->GetAddress();
		if(nat64Present) {
			memcpy(addr.sin6_addr.s6_addr, nat64Prefix, 12);
		} else {
			addr.sin6_addr.s6_addr[11] =
			    addr.sin6_addr.s6_addr[10] = 0xFF;
		}

	} else {
		IPv6Address *v6addr = dynamic_cast<IPv6Address *>
		                      (packet->address);
		assert(v6addr != NULL);
		if(!isAtLeastVista) {
			return;
		}
	}
	addr.sin6_port = htons(packet->port);

	//WSABUF wsaBuf;
	//wsaBuf.buf=(char*) packet->data;
	//wsaBuf.len=packet->length;
	//int res=WSASendTo(fd, &wsaBuf, 1, NULL, 0, (const sockaddr*)&addr, sizeof(addr), NULL, NULL);
	int res = sendto(fd, (char *)packet->data,
	                 packet->length, 0, (sockaddr *)&addr,
	                 sizeof(addr));
	if(res == SOCKET_ERROR) {
		int error = WSAGetLastError();
		LOGE("error sending: %d", error);
		if(error == WSAENETUNREACH && !isV4Available &&
		        VoIPController::GetCurrentTime() < switchToV6at) {
			switchToV6at = VoIPController::GetCurrentTime();
			LOGI("Network unreachable, trying NAT64");
		}
	}
}

void NetworkSocketWinsock::Receive(
    NetworkPacket *packet)
{
	sockaddr_in6 srcAddr;
	sockaddr_in srcAddr4;
	sockaddr *addr;
	int addrLen;
	if(isAtLeastVista) {
		addr = (sockaddr *)&srcAddr;
		addrLen = sizeof(srcAddr);
	} else {
		addr = (sockaddr *)&srcAddr4;
		addrLen = sizeof(srcAddr4);
	}
	//DWORD len;
	//WSABUF buf;
	//buf.buf=(char*) packet->data;
	//buf.len=packet->length;
	//int res=WSARecvFrom(fd, &buf, 1, &len, 0, (sockaddr*) &srcAddr, &addrLen, NULL, NULL);
	int res = recvfrom(fd, (char *)packet->data,
	                   packet->length, 0, addr, &addrLen);
	if(res != SOCKET_ERROR) {
		packet->length = (size_t) res;
	} else {
		packet->length = 0;
		int error = WSAGetLastError();
		LOGE("error receiving: %d", error);
		return;
	}
	//LOGV("Received %d bytes from %s:%d at %.5lf", len, inet_ntoa(srcAddr.sin_addr), ntohs(srcAddr.sin_port), GetCurrentTime());
	if(addr->sa_family == AF_INET) {
		packet->port = srcAddr4.sin_port;
		lastRecvdV4 = IPv4Address(srcAddr4.sin_addr.s_addr);
		packet->address = &lastRecvdV4;
	} else {
		packet->port = srcAddr.sin6_port;
		if(!isV4Available &&
		        IN6_IS_ADDR_V4MAPPED(&srcAddr.sin6_addr)) {
			isV4Available = true;
			LOGI("Detected IPv4 connectivity, will not try IPv6");
		}
		if(IN6_IS_ADDR_V4MAPPED(&srcAddr.sin6_addr) ||
		        (nat64Present &&
		         memcmp(nat64Prefix, srcAddr.sin6_addr.s6_addr,
		                12) == 0)) {
			in_addr v4addr = *((in_addr *)
			                   &srcAddr.sin6_addr.s6_addr[12]);
			lastRecvdV4 = IPv4Address(v4addr.s_addr);
			packet->address = &lastRecvdV4;
		} else {
			lastRecvdV6 = IPv6Address(
			                  srcAddr.sin6_addr.s6_addr);
			packet->address = &lastRecvdV6;
		}
	}
}

void NetworkSocketWinsock::Open()
{
	fd = socket(isAtLeastVista ? AF_INET6 : AF_INET,
	            SOCK_DGRAM, IPPROTO_UDP);
	if(fd == INVALID_SOCKET) {
		int error = WSAGetLastError();
		LOGE("error creating socket: %d", error);
		failed = true;
		return;
	}

	int res;
	if(isAtLeastVista) {
		DWORD flag = 0;
		res = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
		                 (const char *)&flag, sizeof(flag));
		if(res == SOCKET_ERROR) {
			LOGE("error enabling dual stack socket: %d",
			     WSAGetLastError());
			failed = true;
			return;
		}
	}

	SetMaxPriority();

	int tries = 0;
	sockaddr *addr;
	sockaddr_in addr4;
	sockaddr_in6 addr6;
	int addrLen;
	if(isAtLeastVista) {
		//addr.sin6_addr.s_addr=0;
		memset(&addr6, 0, sizeof(sockaddr_in6));
		//addr.sin6_len=sizeof(sa_family_t);
		addr6.sin6_family = AF_INET6;
		addr = (sockaddr *)&addr6;
		addrLen = sizeof(addr6);
	} else {
		sockaddr_in addr4;
		addr4.sin_addr.s_addr = 0;
		addr4.sin_family = AF_INET;
		addr = (sockaddr *)&addr4;
		addrLen = sizeof(addr4);
	}
	for(tries = 0; tries < 10; tries++) {
		uint16_t port = htons(GenerateLocalPort());
		if(isAtLeastVista) {
			((sockaddr_in6 *)addr)->sin6_port = port;
		} else {
			((sockaddr_in *)addr)->sin_port = port;
		}
		res =::bind(fd, addr, addrLen);
		LOGV("trying bind to port %u", ntohs(port));
		if(res < 0) {
			LOGE("error binding to port %u: %d / %s",
			     ntohs(port), errno, strerror(errno));
		} else {
			break;
		}
	}
	if(tries == 10) {
		if(isAtLeastVista) {
			((sockaddr_in6 *)addr)->sin6_port = 0;
		} else {
			((sockaddr_in *)addr)->sin_port = 0;
		}
		res =::bind(fd, addr, addrLen);
		if(res < 0) {
			LOGE("error binding to port %u: %d / %s", 0,
			     errno, strerror(errno));
			//SetState(STATE_FAILED);
			return;
		}
	}
	getsockname(fd, addr, (socklen_t *) &addrLen);
	uint16_t localUdpPort;
	if(isAtLeastVista) {
		localUdpPort = ntohs(((sockaddr_in6 *)
		                      addr)->sin6_port);
	} else {
		localUdpPort = ntohs(((sockaddr_in *)
		                      addr)->sin_port);
	}
	LOGD("Bound to local UDP port %u",
	     ntohs(localUdpPort));

	needUpdateNat64Prefix = true;
	isV4Available = false;
	switchToV6at = VoIPController::GetCurrentTime()
	               + ipv6Timeout;
}

void NetworkSocketWinsock::Close()
{
	closesocket(fd);
}

void NetworkSocketWinsock::OnActiveInterfaceChanged()
{
	needUpdateNat64Prefix = true;
	isV4Available = false;
	switchToV6at = VoIPController::GetCurrentTime()
	               + ipv6Timeout;
}

std::string
NetworkSocketWinsock::GetLocalInterfaceInfo(
    IPv4Address *v4addr, IPv6Address *v6addr)
{
#if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
	Windows::Networking::Connectivity::ConnectionProfile ^
	profile = Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
	if(profile) {
		Windows::Foundation::Collections::IVectorView<Windows::Networking::HostName ^> ^
		hostnames =
		    Windows::Networking::Connectivity::NetworkInformation::GetHostNames();
		for(unsigned int i = 0; i < hostnames->Size; i++) {
			Windows::Networking::HostName ^n =
			    hostnames->GetAt(i);
			if(n->Type !=
			        Windows::Networking::HostNameType::Ipv4 &&
			        n->Type != Windows::Networking::HostNameType::Ipv6) {
				continue;
			}
			if(n->IPInformation->NetworkAdapter->Equals(
			            profile->NetworkAdapter)) {
				if(v4addr &&
				        n->Type == Windows::Networking::HostNameType::Ipv4) {
					char buf[INET_ADDRSTRLEN];
					WideCharToMultiByte(CP_UTF8, 0,
					                    n->RawName->Data(), -1, buf, sizeof(buf), NULL,
					                    NULL);
					*v4addr = IPv4Address(buf);
				} else if(v6addr &&
				          n->Type == Windows::Networking::HostNameType::Ipv6) {
					char buf[INET6_ADDRSTRLEN];
					WideCharToMultiByte(CP_UTF8, 0,
					                    n->RawName->Data(), -1, buf, sizeof(buf), NULL,
					                    NULL);
					*v6addr = IPv6Address(buf);
				}
			}
		}
		char buf[128];
		WideCharToMultiByte(CP_UTF8, 0,
		                    profile->NetworkAdapter->NetworkAdapterId.ToString()->Data(),
		                    -1, buf, sizeof(buf), NULL, NULL);
		return std::string(buf);
	}
	return "";
#else
	IP_ADAPTER_ADDRESSES *addrs =
	    (IP_ADAPTER_ADDRESSES *)malloc(15 * 1024);
	ULONG size = 15 * 1024;
	ULONG flags = GAA_FLAG_SKIP_ANYCAST |
	              GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER
	              | GAA_FLAG_SKIP_FRIENDLY_NAME;

	ULONG res = GetAdaptersAddresses(AF_UNSPEC, flags,
	                                 NULL, addrs, &size);
	if(res == ERROR_BUFFER_OVERFLOW) {
		addrs = (IP_ADAPTER_ADDRESSES *)realloc(addrs, size);
		res = GetAdaptersAddresses(AF_UNSPEC, flags, NULL,
		                           addrs, &size);
	}

	ULONG bestMetric = 0;
	std::string bestName("");

	if(res == ERROR_SUCCESS) {
		IP_ADAPTER_ADDRESSES *current = addrs;
		while(current) {
			char *name = current->AdapterName;
			LOGV("Adapter '%s':", name);
			IP_ADAPTER_UNICAST_ADDRESS *curAddr =
			    current->FirstUnicastAddress;
			if(current->OperStatus != IfOperStatusUp) {
				LOGV("-> (down)");
				current = current->Next;
				continue;
			}
			if(current->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
				LOGV("-> (loopback)");
				current = current->Next;
				continue;
			}
			if(isAtLeastVista) {
				LOGV("v4 metric: %u, v6 metric: %u",
				     current->Ipv4Metric, current->Ipv6Metric);
			}
			while(curAddr) {
				sockaddr *addr = curAddr->Address.lpSockaddr;
				if(addr->sa_family == AF_INET && v4addr) {
					sockaddr_in *ipv4 = (sockaddr_in *)addr;
					LOGV("-> V4: %s",
					     V4AddressToString(ipv4->sin_addr.s_addr).c_str());
					uint32_t ip = ntohl(ipv4->sin_addr.s_addr);
					if((ip & 0xFFFF0000) != 0xA9FE0000) {
						if(isAtLeastVista) {
							if(current->Ipv4Metric > bestMetric) {
								bestMetric = current->Ipv4Metric;
								bestName = std::string(current->AdapterName);
								*v4addr = IPv4Address(ipv4->sin_addr.s_addr);
							}
						} else {
							bestName = std::string(current->AdapterName);
							*v4addr = IPv4Address(ipv4->sin_addr.s_addr);
						}
					}
				} else if(addr->sa_family == AF_INET6 && v6addr) {
					sockaddr_in6 *ipv6 = (sockaddr_in6 *)addr;
					LOGV("-> V6: %s",
					     V6AddressToString(
					         ipv6->sin6_addr.s6_addr).c_str());
					if(!IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) {
						*v6addr = IPv6Address(ipv6->sin6_addr.s6_addr);
					}
				}
				curAddr = curAddr->Next;
			}
			current = current->Next;
		}
	}

	free(addrs);
	return bestName;
#endif
}

uint16_t NetworkSocketWinsock::GetLocalPort()
{
	if(!isAtLeastVista) {
		sockaddr_in addr;
		size_t addrLen = sizeof(sockaddr_in);
		getsockname(fd, (sockaddr *)&addr,
		            (socklen_t *)&addrLen);
		return ntohs(addr.sin_port);
	}
	sockaddr_in6 addr;
	size_t addrLen = sizeof(sockaddr_in6);
	getsockname(fd, (sockaddr *)&addr,
	            (socklen_t *) &addrLen);
	return ntohs(addr.sin6_port);
}

std::string
NetworkSocketWinsock::V4AddressToString(
    uint32_t address)
{
	char buf[INET_ADDRSTRLEN];
	sockaddr_in addr;
	ZeroMemory(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = address;
	DWORD len = sizeof(buf);
	WSAAddressToStringA((sockaddr *)&addr,
	                    sizeof(addr), NULL, buf, &len);
	return std::string(buf);
}

std::string
NetworkSocketWinsock::V6AddressToString(
    unsigned char *address)
{
	char buf[INET6_ADDRSTRLEN];
	sockaddr_in6 addr;
	ZeroMemory(&addr, sizeof(addr));
	addr.sin6_family = AF_INET6;
	memcpy(addr.sin6_addr.s6_addr, address, 16);
	DWORD len = sizeof(buf);
	WSAAddressToStringA((sockaddr *)&addr,
	                    sizeof(addr), NULL, buf, &len);
	return std::string(buf);
}

uint32_t NetworkSocketWinsock::StringToV4Address(
    std::string address)
{
	sockaddr_in addr;
	ZeroMemory(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	int size = sizeof(addr);
	WSAStringToAddressA((char *)address.c_str(),
	                    AF_INET, NULL, (sockaddr *)&addr, &size);
	return addr.sin_addr.s_addr;
}

void NetworkSocketWinsock::StringToV6Address(
    std::string address, unsigned char *out)
{
	sockaddr_in6 addr;
	ZeroMemory(&addr, sizeof(addr));
	addr.sin6_family = AF_INET6;
	int size = sizeof(addr);
	WSAStringToAddressA((char *)address.c_str(),
	                    AF_INET6, NULL, (sockaddr *)&addr, &size);
	memcpy(out, addr.sin6_addr.s6_addr, 16);
}
