#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <cstdint>

struct pcap;
using pcap_t = pcap;
using bpf_u_int32 = unsigned int;
using u_char = unsigned char;

struct pcap_addr
{
    pcap_addr* next;
    sockaddr* addr;
    sockaddr* netmask;
    sockaddr* broadaddr;
    sockaddr* dstaddr;
};
using pcap_addr_t = pcap_addr;

struct pcap_if
{
    pcap_if* next;
    char* name;
    char* description;
    pcap_addr_t* addresses;
    unsigned int flags;
};
using pcap_if_t = pcap_if;

struct pcap_pkthdr
{
    timeval ts;
    bpf_u_int32 caplen;
    bpf_u_int32 len;
};

struct NpcapApi
{
    HMODULE module{};
    int (__cdecl* findalldevs)(pcap_if_t**, char*){};
    void (__cdecl* freealldevs)(pcap_if_t*){};
    pcap_t* (__cdecl* open_live)(const char*, int, int, int, char*){};
    int (__cdecl* next_ex)(pcap_t*, pcap_pkthdr**, const u_char**){};
    void (__cdecl* close)(pcap_t*){};
    int (__cdecl* datalink)(pcap_t*){};

    bool Load();
    void Unload();
};
