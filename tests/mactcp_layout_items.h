#define LAYOUT_ITEMS(X) \
    X(sizeof(wdsEntry)) \
    X(offsetof(wdsEntry, length)) \
    X(offsetof(wdsEntry, ptr)) \
    X(sizeof(rdsEntry)) \
    X(offsetof(rdsEntry, length)) \
    X(offsetof(rdsEntry, ptr)) \
    X(sizeof(ICMPReport)) \
    X(offsetof(ICMPReport, streamPtr)) \
    X(offsetof(ICMPReport, localHost)) \
    X(offsetof(ICMPReport, localPort)) \
    X(offsetof(ICMPReport, remoteHost)) \
    X(offsetof(ICMPReport, remotePort)) \
    X(offsetof(ICMPReport, reportType)) \
    X(offsetof(ICMPReport, optionalAddlInfo)) \
    X(offsetof(ICMPReport, optionalAddlInfoPtr)) \
    X(sizeof(TCPCreatePB)) \
    X(offsetof(TCPCreatePB, rcvBuff)) \
    X(offsetof(TCPCreatePB, rcvBuffLen)) \
    X(offsetof(TCPCreatePB, notifyProc)) \
    X(offsetof(TCPCreatePB, userDataPtr)) \
    X(sizeof(TCPOpenPB)) \
    X(offsetof(TCPOpenPB, ulpTimeoutValue)) \
    X(offsetof(TCPOpenPB, ulpTimeoutAction)) \
    X(offsetof(TCPOpenPB, validityFlags)) \
    X(offsetof(TCPOpenPB, commandTimeoutValue)) \
    X(offsetof(TCPOpenPB, remoteHost)) \
    X(offsetof(TCPOpenPB, remotePort)) \
    X(offsetof(TCPOpenPB, localHost)) \
    X(offsetof(TCPOpenPB, localPort)) \
    X(offsetof(TCPOpenPB, tosFlags)) \
    X(offsetof(TCPOpenPB, precedence)) \
    X(offsetof(TCPOpenPB, dontFrag)) \
    X(offsetof(TCPOpenPB, timeToLive)) \
    X(offsetof(TCPOpenPB, security)) \
    X(offsetof(TCPOpenPB, optionCnt)) \
    X(offsetof(TCPOpenPB, options)) \
    X(offsetof(TCPOpenPB, userDataPtr)) \
    X(sizeof(TCPSendPB)) \
    X(offsetof(TCPSendPB, ulpTimeoutValue)) \
    X(offsetof(TCPSendPB, ulpTimeoutAction)) \
    X(offsetof(TCPSendPB, validityFlags)) \
    X(offsetof(TCPSendPB, pushFlag)) \
    X(offsetof(TCPSendPB, urgentFlag)) \
    X(offsetof(TCPSendPB, filler)) \
    X(offsetof(TCPSendPB, wdsPtr)) \
    X(offsetof(TCPSendPB, sendFree)) \
    X(offsetof(TCPSendPB, sendLength)) \
    X(offsetof(TCPSendPB, userDataPtr)) \
    X(sizeof(TCPReceivePB)) \
    X(offsetof(TCPReceivePB, commandTimeoutValue)) \
    X(offsetof(TCPReceivePB, markFlag)) \
    X(offsetof(TCPReceivePB, urgentFlag)) \
    X(offsetof(TCPReceivePB, filler)) \
    X(offsetof(TCPReceivePB, rcvBuff)) \
    X(offsetof(TCPReceivePB, rcvBuffLen)) \
    X(offsetof(TCPReceivePB, rdsPtr)) \
    X(offsetof(TCPReceivePB, rdsLength)) \
    X(offsetof(TCPReceivePB, secondTimeStamp)) \
    X(offsetof(TCPReceivePB, userDataPtr)) \
    X(sizeof(TCPClosePB)) \
    X(offsetof(TCPClosePB, ulpTimeoutValue)) \
    X(offsetof(TCPClosePB, ulpTimeoutAction)) \
    X(offsetof(TCPClosePB, validityFlags)) \
    X(offsetof(TCPClosePB, filler)) \
    X(offsetof(TCPClosePB, userDataPtr)) \
    X(sizeof(HistoBucket)) \
    X(offsetof(HistoBucket, value)) \
    X(offsetof(HistoBucket, counter)) \
    X(sizeof(TCPConnectionStats)) \
    X(offsetof(TCPConnectionStats, dataPktsRcvd)) \
    X(offsetof(TCPConnectionStats, dataPktsSent)) \
    X(offsetof(TCPConnectionStats, dataPktsResent)) \
    X(offsetof(TCPConnectionStats, bytesRcvd)) \
    X(offsetof(TCPConnectionStats, bytesRcvdDup)) \
    X(offsetof(TCPConnectionStats, bytesRcvdPastWindow)) \
    X(offsetof(TCPConnectionStats, bytesSent)) \
    X(offsetof(TCPConnectionStats, bytesResent)) \
    X(offsetof(TCPConnectionStats, numHistoBuckets)) \
    X(offsetof(TCPConnectionStats, sentSizeHisto)) \
    X(offsetof(TCPConnectionStats, lastRTT)) \
    X(offsetof(TCPConnectionStats, tmrSRTT)) \
    X(offsetof(TCPConnectionStats, rttVariance)) \
    X(offsetof(TCPConnectionStats, tmrRTO)) \
    X(offsetof(TCPConnectionStats, sendTries)) \
    X(offsetof(TCPConnectionStats, sourchQuenchRcvd)) \
    X(sizeof(TCPStatusPB)) \
    X(offsetof(TCPStatusPB, ulpTimeoutValue)) \
    X(offsetof(TCPStatusPB, ulpTimeoutAction)) \
    X(offsetof(TCPStatusPB, unused)) \
    X(offsetof(TCPStatusPB, remoteHost)) \
    X(offsetof(TCPStatusPB, remotePort)) \
    X(offsetof(TCPStatusPB, localHost)) \
    X(offsetof(TCPStatusPB, localPort)) \
    X(offsetof(TCPStatusPB, tosFlags)) \
    X(offsetof(TCPStatusPB, precedence)) \
    X(offsetof(TCPStatusPB, connectionState)) \
    X(offsetof(TCPStatusPB, filler)) \
    X(offsetof(TCPStatusPB, sendWindow)) \
    X(offsetof(TCPStatusPB, rcvWindow)) \
    X(offsetof(TCPStatusPB, amtUnackedData)) \
    X(offsetof(TCPStatusPB, amtUnreadData)) \
    X(offsetof(TCPStatusPB, securityLevelPtr)) \
    X(offsetof(TCPStatusPB, sendUnacked)) \
    X(offsetof(TCPStatusPB, sendNext)) \
    X(offsetof(TCPStatusPB, congestionWindow)) \
    X(offsetof(TCPStatusPB, rcvNext)) \
    X(offsetof(TCPStatusPB, srtt)) \
    X(offsetof(TCPStatusPB, lastRTT)) \
    X(offsetof(TCPStatusPB, sendMaxSegSize)) \
    X(offsetof(TCPStatusPB, connStatPtr)) \
    X(offsetof(TCPStatusPB, userDataPtr)) \
    X(sizeof(TCPAbortPB)) \
    X(offsetof(TCPAbortPB, userDataPtr)) \
    X(sizeof(TCPParam)) \
    X(offsetof(TCPParam, tcpRtoA)) \
    X(offsetof(TCPParam, tcpRtoMin)) \
    X(offsetof(TCPParam, tcpRtoMax)) \
    X(offsetof(TCPParam, tcpMaxSegSize)) \
    X(offsetof(TCPParam, tcpMaxConn)) \
    X(offsetof(TCPParam, tcpMaxWindow)) \
    X(sizeof(TCPStats)) \
    X(offsetof(TCPStats, tcpConnAttempts)) \
    X(offsetof(TCPStats, tcpConnOpened)) \
    X(offsetof(TCPStats, tcpConnAccepted)) \
    X(offsetof(TCPStats, tcpConnClosed)) \
    X(offsetof(TCPStats, tcpConnAborted)) \
    X(offsetof(TCPStats, tcpOctetsIn)) \
    X(offsetof(TCPStats, tcpOctetsOut)) \
    X(offsetof(TCPStats, tcpOctetsInDup)) \
    X(offsetof(TCPStats, tcpOctetsRetrans)) \
    X(offsetof(TCPStats, tcpInputPkts)) \
    X(offsetof(TCPStats, tcpOutputPkts)) \
    X(offsetof(TCPStats, tcpDupPkts)) \
    X(offsetof(TCPStats, tcpRetransPkts)) \
    X(sizeof(TCPGlobalInfoPB)) \
    X(offsetof(TCPGlobalInfoPB, tcpParamPtr)) \
    X(offsetof(TCPGlobalInfoPB, tcpStatsPtr)) \
    X(offsetof(TCPGlobalInfoPB, tcpCDBTable)) \
    X(offsetof(TCPGlobalInfoPB, userDataPtr)) \
    X(offsetof(TCPGlobalInfoPB, maxTCPConnections)) \
    X(sizeof(TCPiopb)) \
    X(offsetof(TCPiopb, fill12)) \
    X(offsetof(TCPiopb, ioCompletion)) \
    X(offsetof(TCPiopb, ioResult)) \
    X(offsetof(TCPiopb, ioNamePtr)) \
    X(offsetof(TCPiopb, ioVRefNum)) \
    X(offsetof(TCPiopb, ioCRefNum)) \
    X(offsetof(TCPiopb, csCode)) \
    X(offsetof(TCPiopb, tcpStream)) \
    X(offsetof(TCPiopb, csParam.create.rcvBuff)) \
    X(offsetof(TCPiopb, csParam.create.rcvBuffLen)) \
    X(offsetof(TCPiopb, csParam.create.notifyProc)) \
    X(offsetof(TCPiopb, csParam.create.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.open.ulpTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.open.ulpTimeoutAction)) \
    X(offsetof(TCPiopb, csParam.open.validityFlags)) \
    X(offsetof(TCPiopb, csParam.open.commandTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.open.remoteHost)) \
    X(offsetof(TCPiopb, csParam.open.remotePort)) \
    X(offsetof(TCPiopb, csParam.open.localHost)) \
    X(offsetof(TCPiopb, csParam.open.localPort)) \
    X(offsetof(TCPiopb, csParam.open.tosFlags)) \
    X(offsetof(TCPiopb, csParam.open.precedence)) \
    X(offsetof(TCPiopb, csParam.open.dontFrag)) \
    X(offsetof(TCPiopb, csParam.open.timeToLive)) \
    X(offsetof(TCPiopb, csParam.open.security)) \
    X(offsetof(TCPiopb, csParam.open.optionCnt)) \
    X(offsetof(TCPiopb, csParam.open.options)) \
    X(offsetof(TCPiopb, csParam.open.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.send.ulpTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.send.ulpTimeoutAction)) \
    X(offsetof(TCPiopb, csParam.send.validityFlags)) \
    X(offsetof(TCPiopb, csParam.send.pushFlag)) \
    X(offsetof(TCPiopb, csParam.send.urgentFlag)) \
    X(offsetof(TCPiopb, csParam.send.filler)) \
    X(offsetof(TCPiopb, csParam.send.wdsPtr)) \
    X(offsetof(TCPiopb, csParam.send.sendFree)) \
    X(offsetof(TCPiopb, csParam.send.sendLength)) \
    X(offsetof(TCPiopb, csParam.send.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.receive.commandTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.receive.markFlag)) \
    X(offsetof(TCPiopb, csParam.receive.urgentFlag)) \
    X(offsetof(TCPiopb, csParam.receive.filler)) \
    X(offsetof(TCPiopb, csParam.receive.rcvBuff)) \
    X(offsetof(TCPiopb, csParam.receive.rcvBuffLen)) \
    X(offsetof(TCPiopb, csParam.receive.rdsPtr)) \
    X(offsetof(TCPiopb, csParam.receive.rdsLength)) \
    X(offsetof(TCPiopb, csParam.receive.secondTimeStamp)) \
    X(offsetof(TCPiopb, csParam.receive.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.close.ulpTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.close.ulpTimeoutAction)) \
    X(offsetof(TCPiopb, csParam.close.validityFlags)) \
    X(offsetof(TCPiopb, csParam.close.filler)) \
    X(offsetof(TCPiopb, csParam.close.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.abort.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.status.ulpTimeoutValue)) \
    X(offsetof(TCPiopb, csParam.status.ulpTimeoutAction)) \
    X(offsetof(TCPiopb, csParam.status.unused)) \
    X(offsetof(TCPiopb, csParam.status.remoteHost)) \
    X(offsetof(TCPiopb, csParam.status.remotePort)) \
    X(offsetof(TCPiopb, csParam.status.localHost)) \
    X(offsetof(TCPiopb, csParam.status.localPort)) \
    X(offsetof(TCPiopb, csParam.status.tosFlags)) \
    X(offsetof(TCPiopb, csParam.status.precedence)) \
    X(offsetof(TCPiopb, csParam.status.connectionState)) \
    X(offsetof(TCPiopb, csParam.status.filler)) \
    X(offsetof(TCPiopb, csParam.status.sendWindow)) \
    X(offsetof(TCPiopb, csParam.status.rcvWindow)) \
    X(offsetof(TCPiopb, csParam.status.amtUnackedData)) \
    X(offsetof(TCPiopb, csParam.status.amtUnreadData)) \
    X(offsetof(TCPiopb, csParam.status.securityLevelPtr)) \
    X(offsetof(TCPiopb, csParam.status.sendUnacked)) \
    X(offsetof(TCPiopb, csParam.status.sendNext)) \
    X(offsetof(TCPiopb, csParam.status.congestionWindow)) \
    X(offsetof(TCPiopb, csParam.status.rcvNext)) \
    X(offsetof(TCPiopb, csParam.status.srtt)) \
    X(offsetof(TCPiopb, csParam.status.lastRTT)) \
    X(offsetof(TCPiopb, csParam.status.sendMaxSegSize)) \
    X(offsetof(TCPiopb, csParam.status.connStatPtr)) \
    X(offsetof(TCPiopb, csParam.status.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.globalInfo.tcpParamPtr)) \
    X(offsetof(TCPiopb, csParam.globalInfo.tcpStatsPtr)) \
    X(offsetof(TCPiopb, csParam.globalInfo.tcpCDBTable)) \
    X(offsetof(TCPiopb, csParam.globalInfo.userDataPtr)) \
    X(offsetof(TCPiopb, csParam.globalInfo.maxTCPConnections)) \
    X(sizeof(UDPCreatePB)) \
    X(offsetof(UDPCreatePB, rcvBuff)) \
    X(offsetof(UDPCreatePB, rcvBuffLen)) \
    X(offsetof(UDPCreatePB, notifyProc)) \
    X(offsetof(UDPCreatePB, localPort)) \
    X(offsetof(UDPCreatePB, userDataPtr)) \
    X(offsetof(UDPCreatePB, endingPort)) \
    X(sizeof(UDPSendPB)) \
    X(offsetof(UDPSendPB, reserved)) \
    X(offsetof(UDPSendPB, remoteHost)) \
    X(offsetof(UDPSendPB, remotePort)) \
    X(offsetof(UDPSendPB, wdsPtr)) \
    X(offsetof(UDPSendPB, checkSum)) \
    X(offsetof(UDPSendPB, filler)) \
    X(offsetof(UDPSendPB, sendLength)) \
    X(offsetof(UDPSendPB, userDataPtr)) \
    X(offsetof(UDPSendPB, localPort)) \
    X(sizeof(UDPReceivePB)) \
    X(offsetof(UDPReceivePB, timeOut)) \
    X(offsetof(UDPReceivePB, remoteHost)) \
    X(offsetof(UDPReceivePB, remotePort)) \
    X(offsetof(UDPReceivePB, rcvBuff)) \
    X(offsetof(UDPReceivePB, rcvBuffLen)) \
    X(offsetof(UDPReceivePB, secondTimeStamp)) \
    X(offsetof(UDPReceivePB, userDataPtr)) \
    X(offsetof(UDPReceivePB, destHost)) \
    X(offsetof(UDPReceivePB, destPort)) \
    X(sizeof(UDPMTUPB)) \
    X(offsetof(UDPMTUPB, mtuSize)) \
    X(offsetof(UDPMTUPB, remoteHost)) \
    X(offsetof(UDPMTUPB, userDataPtr)) \
    X(sizeof(UDPiopb)) \
    X(offsetof(UDPiopb, fill12)) \
    X(offsetof(UDPiopb, ioCompletion)) \
    X(offsetof(UDPiopb, ioResult)) \
    X(offsetof(UDPiopb, ioNamePtr)) \
    X(offsetof(UDPiopb, ioVRefNum)) \
    X(offsetof(UDPiopb, ioCRefNum)) \
    X(offsetof(UDPiopb, csCode)) \
    X(offsetof(UDPiopb, udpStream)) \
    X(offsetof(UDPiopb, csParam.create.rcvBuff)) \
    X(offsetof(UDPiopb, csParam.create.rcvBuffLen)) \
    X(offsetof(UDPiopb, csParam.create.notifyProc)) \
    X(offsetof(UDPiopb, csParam.create.localPort)) \
    X(offsetof(UDPiopb, csParam.create.userDataPtr)) \
    X(offsetof(UDPiopb, csParam.create.endingPort)) \
    X(offsetof(UDPiopb, csParam.send.reserved)) \
    X(offsetof(UDPiopb, csParam.send.remoteHost)) \
    X(offsetof(UDPiopb, csParam.send.remotePort)) \
    X(offsetof(UDPiopb, csParam.send.wdsPtr)) \
    X(offsetof(UDPiopb, csParam.send.checkSum)) \
    X(offsetof(UDPiopb, csParam.send.filler)) \
    X(offsetof(UDPiopb, csParam.send.sendLength)) \
    X(offsetof(UDPiopb, csParam.send.userDataPtr)) \
    X(offsetof(UDPiopb, csParam.send.localPort)) \
    X(offsetof(UDPiopb, csParam.receive.timeOut)) \
    X(offsetof(UDPiopb, csParam.receive.remoteHost)) \
    X(offsetof(UDPiopb, csParam.receive.remotePort)) \
    X(offsetof(UDPiopb, csParam.receive.rcvBuff)) \
    X(offsetof(UDPiopb, csParam.receive.rcvBuffLen)) \
    X(offsetof(UDPiopb, csParam.receive.secondTimeStamp)) \
    X(offsetof(UDPiopb, csParam.receive.userDataPtr)) \
    X(offsetof(UDPiopb, csParam.receive.destHost)) \
    X(offsetof(UDPiopb, csParam.receive.destPort)) \
    X(offsetof(UDPiopb, csParam.mtu.mtuSize)) \
    X(offsetof(UDPiopb, csParam.mtu.remoteHost)) \
    X(offsetof(UDPiopb, csParam.mtu.userDataPtr)) \
    X(sizeof(GetAddrParamBlock)) \
    X(offsetof(GetAddrParamBlock, qLink)) \
    X(offsetof(GetAddrParamBlock, qType)) \
    X(offsetof(GetAddrParamBlock, ioTrap)) \
    X(offsetof(GetAddrParamBlock, ioCmdAddr)) \
    X(offsetof(GetAddrParamBlock, ioCompletion)) \
    X(offsetof(GetAddrParamBlock, ioResult)) \
    X(offsetof(GetAddrParamBlock, ioNamePtr)) \
    X(offsetof(GetAddrParamBlock, ioVRefNum)) \
    X(offsetof(GetAddrParamBlock, ioCRefNum)) \
    X(offsetof(GetAddrParamBlock, csCode)) \
    X(offsetof(GetAddrParamBlock, ourAddress)) \
    X(offsetof(GetAddrParamBlock, ourNetMask)) \
    X(sizeof(ICMPParamBlock)) \
    X(offsetof(ICMPParamBlock, qLink)) \
    X(offsetof(ICMPParamBlock, qType)) \
    X(offsetof(ICMPParamBlock, ioTrap)) \
    X(offsetof(ICMPParamBlock, ioCmdAddr)) \
    X(offsetof(ICMPParamBlock, ioCompletion)) \
    X(offsetof(ICMPParamBlock, ioResult)) \
    X(offsetof(ICMPParamBlock, ioNamePtr)) \
    X(offsetof(ICMPParamBlock, ioVRefNum)) \
    X(offsetof(ICMPParamBlock, ioCRefNum)) \
    X(offsetof(ICMPParamBlock, csCode)) \
    X(offsetof(ICMPParamBlock, params)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo)) \
    X(sizeof(IPParamBlock)) \
    X(offsetof(IPParamBlock, qLink)) \
    X(offsetof(IPParamBlock, qType)) \
    X(offsetof(IPParamBlock, ioTrap)) \
    X(offsetof(IPParamBlock, ioCmdAddr)) \
    X(offsetof(IPParamBlock, ioCompletion)) \
    X(offsetof(IPParamBlock, ioResult)) \
    X(offsetof(IPParamBlock, ioNamePtr)) \
    X(offsetof(IPParamBlock, ioVRefNum)) \
    X(offsetof(IPParamBlock, ioCRefNum)) \
    X(offsetof(IPParamBlock, csCode)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.dest)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.data)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.timeout)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.options)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.optLength)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.icmpCompletion)) \
    X(offsetof(IPParamBlock, csParam.IPEchoPB.userDataPtr)) \
    X(offsetof(IPParamBlock, csParam.LAPStatsPB.lapStatsPtr)) \
    X(sizeof(LAPStats)) \
    X(offsetof(LAPStats, ifType)) \
    X(offsetof(LAPStats, ifString)) \
    X(offsetof(LAPStats, ifMaxMTU)) \
    X(offsetof(LAPStats, ifSpeed)) \
    X(offsetof(LAPStats, ifPhyAddrLength)) \
    X(offsetof(LAPStats, ifPhysicalAddress)) \
    X(offsetof(LAPStats, AddrXlation)) \
    X(offsetof(LAPStats, slotNumber)) \
    X(sizeof(Enet_addr)) \
    X(offsetof(Enet_addr, en_hi)) \
    X(offsetof(Enet_addr, en_lo)) \
    X(sizeof(arp_entry)) \
    X(offsetof(arp_entry, age)) \
    X(offsetof(arp_entry, protocol)) \
    X(offsetof(arp_entry, ip_address)) \
    X(offsetof(arp_entry, en_address)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo.echoRequestOut)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo.echoReplyIn)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo.echoedData)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo.options)) \
    X(offsetof(ICMPParamBlock, icmpEchoInfo.userDataPtr)) \
    X(sizeof(((TCPiopb *)0)->csParam)) \
    X(sizeof(((UDPiopb *)0)->csParam)) \
    X(sizeof(((IPParamBlock *)0)->csParam)) \
    /* end */

#define CONST_ITEMS(X) \
    X(inProgress) \
    X(ipBadLapErr) \
    X(ipBadCnfgErr) \
    X(ipNoCnfgErr) \
    X(ipLoadErr) \
    X(ipBadAddr) \
    X(connectionClosing) \
    X(invalidLength) \
    X(connectionExists) \
    X(connectionDoesntExist) \
    X(insufficientResources) \
    X(invalidStreamPtr) \
    X(streamAlreadyOpen) \
    X(connectionTerminated) \
    X(invalidBufPtr) \
    X(invalidRDS) \
    X(invalidWDS) \
    X(openFailed) \
    X(commandTimeout) \
    X(duplicateSocket) \
    X(ipDontFragErr) \
    X(ipDestDeadErr) \
    X(icmpEchoTimeoutErr) \
    X(ipNoFragMemErr) \
    X(ipRouteErr) \
    X(nameSyntaxErr) \
    X(cacheFault) \
    X(noResultProc) \
    X(noNameServer) \
    X(authNameErr) \
    X(noAnsErr) \
    X(dnrErr) \
    X(outOfMemory) \
    X(ipctlGetAddr) \
    X(ipctlEchoICMP) \
    X(ipctlLAPStats) \
    X(UDPCreate) \
    X(UDPRead) \
    X(UDPBfrReturn) \
    X(UDPWrite) \
    X(UDPRelease) \
    X(UDPMaxMTUSize) \
    X(UDPMultiCreate) \
    X(UDPMultiSend) \
    X(UDPMultiRead) \
    X(UDPCtlMax) \
    X(TCPCreate) \
    X(TCPPassiveOpen) \
    X(TCPActiveOpen) \
    X(TCPSend) \
    X(TCPNoCopyRcv) \
    X(TCPRcvBfrReturn) \
    X(TCPRcv) \
    X(TCPClose) \
    X(TCPAbort) \
    X(TCPStatus) \
    X(TCPExtendedStat) \
    X(TCPRelease) \
    X(TCPGlobalInfo) \
    X(TCPCtlMax) \
    X(TCPClosing) \
    X(TCPULPTimeout) \
    X(TCPTerminate) \
    X(TCPDataArrival) \
    X(TCPUrgent) \
    X(TCPICMPReceived) \
    X(UDPDataArrival) \
    X(UDPICMPReceived) \
    X(TCPRemoteAbort) \
    X(TCPNetworkFailure) \
    X(TCPSecPrecMismatch) \
    X(TCPULPTimeoutTerminate) \
    X(TCPULPAbort) \
    X(TCPULPClose) \
    X(TCPServiceError) \
    X(netUnreach) \
    X(hostUnreach) \
    X(protocolUnreach) \
    X(portUnreach) \
    X(fragReqd) \
    X(sourceRouteFailed) \
    X(timeExceeded) \
    X(parmProblem) \
    X(missingOption) \
    X(timeoutValue) \
    X(timeoutAction) \
    X(typeOfService) \
    X(precedence) \
    X(lowDelay) \
    X(throughPut) \
    X(reliability) \
    X(ConnectionStateClosed) \
    X(ConnectionStateListening) \
    X(ConnectionStateSYNReceived) \
    X(ConnectionStateSYNSent) \
    X(ConnectionStateEstablished) \
    X(ConnectionStateFINWait1) \
    X(ConnectionStateFINWait2) \
    X(ConnectionStateCloseWait) \
    X(ConnectionStateClosing) \
    X(ConnectionStateLastAck) \
    X(ConnectionStateTimeWait) \
    X(NumOfHistoBuckets) \
    X(ARP_TABLE_SIZE) \
    /* end */
