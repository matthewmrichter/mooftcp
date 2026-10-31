/* mt_mactcp.h - MoofTCP's own declaration of the MacTCP application interface.
 *
 * Written from the layouts recorded in docs/dev/MACTCP-INTERFACE.md (ref = taken
 * from the reference interface file, obs = observed). Nothing here is copied
 * text. The names deliberately equal MacTCP's own so app-facing code reads
 * naturally (TCPiopb, wdsEntry, ipctlGetAddr, ...).
 *
 * LAYOUT RULES
 *  - Every field has a fixed width. A 68k pointer is 32 bits, so every
 *    pointer-valued field is declared MacPtr32 (a 32-bit address), never a C
 *    pointer. Glue code converts with its own accessors (mt_os.h). That keeps
 *    the layouts identical on the 68k and on a 64-bit test host.
 *  - 68k compilers align 16/32-bit fields to 2 bytes, so a 32-bit field may
 *    sit at an offset that is 2 mod 4 (TCPiopb.ioNamePtr is at 22). A host
 *    gcc would insert padding there. The header therefore wraps every
 *    structure in #pragma pack(push, 2), which is a no-op on the 68k and
 *    reproduces the 68k layout on the host. tests/test_mactcp_layout.c
 *    proves every size and offset against the reference header.
 *  - Reserved bytes are explicit fields (fill12, filler). Booleans are uint8_t,
 *    a Mac "short" is int16_t, a "long" is int32_t.
 *  - All integers in these blocks are big-endian in memory on the 68k. The
 *    host test only checks layout, not byte order.
 */
#ifndef MT_MACTCP_H
#define MT_MACTCP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t MacPtr32;      /* a 68k address held as a plain 32-bit value */
typedef uint32_t ip_addr;
typedef uint16_t ip_port;
typedef uint16_t tcp_port;
typedef uint16_t udp_port;
typedef uint32_t StreamPtr;     /* opaque 32-bit stream handle */
typedef uint32_t BufferPtr;
typedef uint16_t TCPEventCode;
typedef uint16_t UDPEventCode;
typedef uint16_t TCPTerminationReason;
typedef uint16_t ICMPMsgType;

/* ---- Result codes ---- */
enum {
    inProgress                = 1,      /* ioResult while a call is pending */
    ipBadLapErr               = -23000,
    ipBadCnfgErr              = -23001,
    ipNoCnfgErr               = -23002,
    ipLoadErr                 = -23003,
    ipBadAddr                 = -23004,
    connectionClosing         = -23005,
    invalidLength             = -23006,
    connectionExists          = -23007,
    connectionDoesntExist     = -23008,
    insufficientResources     = -23009,
    invalidStreamPtr          = -23010,
    streamAlreadyOpen         = -23011,
    connectionTerminated      = -23012,
    invalidBufPtr             = -23013,
    invalidRDS                = -23014,
    invalidWDS                = -23014, /* same value as invalidRDS */
    openFailed                = -23015,
    commandTimeout            = -23016,
    duplicateSocket           = -23017
};
enum {
    ipDontFragErr             = -23032,
    ipDestDeadErr             = -23033,
    icmpEchoTimeoutErr        = -23035,
    ipNoFragMemErr            = -23036,
    ipRouteErr                = -23037,
    nameSyntaxErr             = -23041,
    cacheFault                = -23042,
    noResultProc              = -23043,
    noNameServer              = -23044,
    authNameErr               = -23045,
    noAnsErr                  = -23046,
    dnrErr                    = -23047,
    outOfMemory               = -23048
};

/* ---- Control codes ---- */
enum { ipctlGetAddr = 15, ipctlEchoICMP = 17, ipctlLAPStats = 19 };
enum {
    UDPCreate = 20, UDPRead = 21, UDPBfrReturn = 22, UDPWrite = 23,
    UDPRelease = 24, UDPMaxMTUSize = 25, UDPStatus = 26,
    UDPMultiCreate = 27, UDPMultiSend = 28, UDPMultiRead = 29,
    UDPCtlMax = 29
};
enum {
    TCPCreate = 30, TCPPassiveOpen = 31, TCPActiveOpen = 32,
    TCPSend = 34, TCPNoCopyRcv = 35, TCPRcvBfrReturn = 36, TCPRcv = 37,
    TCPClose = 38, TCPAbort = 39, TCPStatus = 40, TCPExtendedStat = 41,
    TCPRelease = 42, TCPGlobalInfo = 43, TCPCtlMax = 49
};

/* ---- Asynchronous notification (ASR) codes ---- */
enum {
    TCPClosing = 1, TCPULPTimeout = 2, TCPTerminate = 3, TCPDataArrival = 4,
    TCPUrgent = 5, TCPICMPReceived = 6
};
enum { UDPDataArrival = 1, UDPICMPReceived = 2 };
enum {
    TCPRemoteAbort = 2, TCPNetworkFailure = 3, TCPSecPrecMismatch = 4,
    TCPULPTimeoutTerminate = 5, TCPULPAbort = 6, TCPULPClose = 7,
    TCPServiceError = 8
};
/* ICMPReport.reportType values */
enum {
    netUnreach = 0, hostUnreach = 1, protocolUnreach = 2, portUnreach = 3,
    fragReqd = 4, sourceRouteFailed = 5, timeExceeded = 6, parmProblem = 7,
    missingOption = 8
};

/* TCPOpenPB / TCPSendPB / TCPClosePB validityFlags and tosFlags bits */
enum { timeoutValue = 0x80, timeoutAction = 0x40, typeOfService = 0x20, precedence = 0x10 };
enum { lowDelay = 0x01, throughPut = 0x02, reliability = 0x04 };

/* TCPStatusPB.connectionState */
enum {
    ConnectionStateClosed = 0, ConnectionStateListening = 2,
    ConnectionStateSYNReceived = 4, ConnectionStateSYNSent = 6,
    ConnectionStateEstablished = 8, ConnectionStateFINWait1 = 10,
    ConnectionStateFINWait2 = 12, ConnectionStateCloseWait = 14,
    ConnectionStateClosing = 16, ConnectionStateLastAck = 18,
    ConnectionStateTimeWait = 20
};

enum { NumOfHistoBuckets = 7, ARP_TABLE_SIZE = 20 };

#pragma pack(push, 2)

/* ---- Shared small structures ---- */
typedef struct wdsEntry { uint16_t length; MacPtr32 ptr; } wdsEntry;   /* write data structure */
typedef struct rdsEntry { uint16_t length; MacPtr32 ptr; } rdsEntry;   /* read data structure */

typedef struct ICMPReport {
    StreamPtr streamPtr;
    ip_addr   localHost;
    ip_port   localPort;
    ip_addr   remoteHost;
    ip_port   remotePort;
    int16_t   reportType;
    uint16_t  optionalAddlInfo;
    uint32_t  optionalAddlInfoPtr;
} ICMPReport;

/* ---- TCP ---- */
typedef struct TCPCreatePB {
    MacPtr32 rcvBuff;
    uint32_t rcvBuffLen;
    MacPtr32 notifyProc;
    MacPtr32 userDataPtr;
} TCPCreatePB;

typedef struct TCPOpenPB {
    int8_t   ulpTimeoutValue;
    int8_t   ulpTimeoutAction;
    int8_t   validityFlags;
    int8_t   commandTimeoutValue;
    ip_addr  remoteHost;
    tcp_port remotePort;
    ip_addr  localHost;
    tcp_port localPort;
    int8_t   tosFlags;
    int8_t   precedence;
    uint8_t  dontFrag;
    int8_t   timeToLive;
    int8_t   security;
    int8_t   optionCnt;
    int8_t   options[40];
    MacPtr32 userDataPtr;
} TCPOpenPB;

typedef struct TCPSendPB {
    int8_t   ulpTimeoutValue;
    int8_t   ulpTimeoutAction;
    int8_t   validityFlags;
    uint8_t  pushFlag;
    uint8_t  urgentFlag;
    int8_t   filler;
    MacPtr32 wdsPtr;
    uint32_t sendFree;
    uint16_t sendLength;
    MacPtr32 userDataPtr;
} TCPSendPB;

typedef struct TCPReceivePB {
    int8_t   commandTimeoutValue;
    uint8_t  markFlag;
    uint8_t  urgentFlag;
    int8_t   filler;
    MacPtr32 rcvBuff;
    uint16_t rcvBuffLen;
    MacPtr32 rdsPtr;
    uint16_t rdsLength;
    uint16_t secondTimeStamp;
    MacPtr32 userDataPtr;
} TCPReceivePB;

typedef struct TCPClosePB {
    int8_t   ulpTimeoutValue;
    int8_t   ulpTimeoutAction;
    int8_t   validityFlags;
    int8_t   filler;
    MacPtr32 userDataPtr;
} TCPClosePB;

typedef struct HistoBucket { uint16_t value; uint32_t counter; } HistoBucket;

typedef struct TCPConnectionStats {
    uint32_t   dataPktsRcvd;
    uint32_t   dataPktsSent;
    uint32_t   dataPktsResent;
    uint32_t   bytesRcvd;
    uint32_t   bytesRcvdDup;
    uint32_t   bytesRcvdPastWindow;
    uint32_t   bytesSent;
    uint32_t   bytesResent;
    uint16_t   numHistoBuckets;
    HistoBucket sentSizeHisto[NumOfHistoBuckets];
    uint16_t   lastRTT;
    uint16_t   tmrSRTT;
    uint16_t   rttVariance;
    uint16_t   tmrRTO;
    int8_t     sendTries;
    int8_t     sourchQuenchRcvd;      /* sic: spelling is the reference's field name */
} TCPConnectionStats;

typedef struct TCPStatusPB {
    int8_t   ulpTimeoutValue;
    int8_t   ulpTimeoutAction;
    int32_t  unused;
    ip_addr  remoteHost;
    tcp_port remotePort;
    ip_addr  localHost;
    tcp_port localPort;
    int8_t   tosFlags;
    int8_t   precedence;
    int8_t   connectionState;
    int8_t   filler;
    uint16_t sendWindow;
    uint16_t rcvWindow;
    uint16_t amtUnackedData;
    uint16_t amtUnreadData;
    MacPtr32 securityLevelPtr;
    uint32_t sendUnacked;
    uint32_t sendNext;
    uint32_t congestionWindow;
    uint32_t rcvNext;
    uint32_t srtt;
    uint32_t lastRTT;
    uint32_t sendMaxSegSize;
    MacPtr32 connStatPtr;
    MacPtr32 userDataPtr;
} TCPStatusPB;

typedef struct TCPAbortPB { MacPtr32 userDataPtr; } TCPAbortPB;

typedef struct TCPParam {
    uint32_t tcpRtoA, tcpRtoMin, tcpRtoMax, tcpMaxSegSize, tcpMaxConn, tcpMaxWindow;
} TCPParam;

typedef struct TCPStats {
    uint32_t tcpConnAttempts, tcpConnOpened, tcpConnAccepted, tcpConnClosed,
             tcpConnAborted, tcpOctetsIn, tcpOctetsOut, tcpOctetsInDup,
             tcpOctetsRetrans, tcpInputPkts, tcpOutputPkts, tcpDupPkts,
             tcpRetransPkts;
} TCPStats;

typedef struct TCPGlobalInfoPB {
    MacPtr32 tcpParamPtr;
    MacPtr32 tcpStatsPtr;
    MacPtr32 tcpCDBTable[1];          /* first element of a table of stream pointers */
    MacPtr32 userDataPtr;
    uint16_t maxTCPConnections;
} TCPGlobalInfoPB;

typedef struct TCPiopb {
    int8_t   fill12[12];              /* queue link, type, trap, cmd address: driver-owned */
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
    StreamPtr tcpStream;
    union {
        TCPCreatePB     create;
        TCPOpenPB       open;
        TCPSendPB       send;
        TCPReceivePB    receive;
        TCPClosePB      close;
        TCPAbortPB      abort;
        TCPStatusPB     status;
        TCPGlobalInfoPB globalInfo;
    } csParam;
} TCPiopb;

/* ---- UDP ---- */
typedef struct UDPCreatePB {
    MacPtr32 rcvBuff;
    uint32_t rcvBuffLen;
    MacPtr32 notifyProc;
    uint16_t localPort;
    MacPtr32 userDataPtr;
    udp_port endingPort;
} UDPCreatePB;

typedef struct UDPSendPB {
    uint16_t reserved;
    ip_addr  remoteHost;
    udp_port remotePort;
    MacPtr32 wdsPtr;
    uint8_t  checkSum;
    int8_t   filler;
    uint16_t sendLength;
    MacPtr32 userDataPtr;
    udp_port localPort;
} UDPSendPB;

typedef struct UDPReceivePB {
    uint16_t timeOut;
    ip_addr  remoteHost;
    udp_port remotePort;
    MacPtr32 rcvBuff;
    uint16_t rcvBuffLen;
    uint16_t secondTimeStamp;
    MacPtr32 userDataPtr;
    ip_addr  destHost;                /* multi-receive only */
    udp_port destPort;                /* multi-receive only */
} UDPReceivePB;

typedef struct UDPMTUPB {
    uint16_t mtuSize;
    ip_addr  remoteHost;
    MacPtr32 userDataPtr;
} UDPMTUPB;

typedef struct UDPiopb {
    int8_t   fill12[12];
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
    StreamPtr udpStream;
    union {
        UDPCreatePB  create;
        UDPSendPB    send;
        UDPReceivePB receive;
        UDPMTUPB     mtu;
    } csParam;
} UDPiopb;

/* ---- IP / ICMP / address ---- */
typedef struct GetAddrParamBlock {
    MacPtr32 qLink;
    int16_t  qType;
    int16_t  ioTrap;
    MacPtr32 ioCmdAddr;
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
    ip_addr  ourAddress;
    int32_t  ourNetMask;
} GetAddrParamBlock;

typedef struct ICMPEchoInfo {
    uint32_t echoRequestOut;          /* ticks when the request went out */
    uint32_t echoReplyIn;             /* ticks when the reply arrived */
    rdsEntry echoedData;
    MacPtr32 options;
    uint32_t userDataPtr;
} ICMPEchoInfo;

typedef struct ICMPParamBlock {
    MacPtr32 qLink;
    int16_t  qType;
    int16_t  ioTrap;
    MacPtr32 ioCmdAddr;
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
    int16_t  params[11];
    ICMPEchoInfo icmpEchoInfo;
} ICMPParamBlock;

typedef struct IPEchoPB {
    ip_addr  dest;
    wdsEntry data;
    int16_t  timeout;
    MacPtr32 options;
    uint16_t optLength;
    MacPtr32 icmpCompletion;
    uint32_t userDataPtr;
} IPEchoPB;

typedef struct LAPStatsPB { MacPtr32 lapStatsPtr; } LAPStatsPB;

typedef struct IPParamBlock {
    MacPtr32 qLink;
    int16_t  qType;
    int16_t  ioTrap;
    MacPtr32 ioCmdAddr;
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
    union {
        IPEchoPB   IPEchoPB;
        LAPStatsPB LAPStatsPB;
    } csParam;
} IPParamBlock;

typedef struct LAPStats {
    int16_t  ifType;
    MacPtr32 ifString;
    int16_t  ifMaxMTU;
    int32_t  ifSpeed;
    int16_t  ifPhyAddrLength;
    MacPtr32 ifPhysicalAddress;
    MacPtr32 AddrXlation;             /* pointer to the ARP table */
    int16_t  slotNumber;
} LAPStats;

typedef struct Enet_addr { uint16_t en_hi; uint32_t en_lo; } Enet_addr;

typedef struct arp_entry {
    int16_t   age;
    uint16_t  protocol;
    ip_addr   ip_address;
    Enet_addr en_address;
} arp_entry;

/* ---- DNR (the dnrp code resource) ----
 * obs: the layout our own HeapInfoDNR (mac/heapinfo/dnrheap.c) passes, built
 * with Retro68 (2-byte alignment). addr[] therefore sits at offset 260. */
#define MT_NUM_ALT_ADDRS 4
typedef struct hostInfo {
    int32_t  rtnCode;
    char     cname[255];
    uint32_t addr[MT_NUM_ALT_ADDRS];
} hostInfo;

#pragma pack(pop)

/* ---- DNR selectors (obs, MACTCP-REFERENCE-MATRIX.md "DNR") ----
 * 1-8 are the entries of the 'dnrp' jump table. AddrToStr 4, EnumCache 5, AddrToName 6, HInfo 7
 * and MXInfo 8 are Apple's dnr.c numbers, confirmed against the real dnrp by the ApiProbe DNR group
 * (each selector does what its name says). */
enum { OPENRESOLVER = 1, CLOSERESOLVER = 2, STRTOADDR = 3, ADDRTOSTR = 4, ENUMCACHE = 5,
       ADDRTONAME = 6, HINFO = 7, MXINFO = 8 };

/* returnRec of HInfo / MXInfo (obs: offsets seen through the real dnrp): rtnCode 0, cname 4,
 * then at 260 either cpuType[30] + osType[30] or the MX preference (uint16) + exchange[255]. */
#define MT_RETREC_UNION 260

#ifdef __cplusplus
}
#endif

#endif /* MT_MACTCP_H */
