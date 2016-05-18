/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2007 Georgia Tech Research Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Raj Bhattacharjea <raj.b@gatech.edu>
 */

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/boolean.h"
#include "ns3/object-vector.h"

#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/ipv4-route.h"
#include "ns3/ipv6-route.h"

#include "tcp-l4-protocol.h"
#include "tcp-header.h"
#include "ipv4-end-point-demux.h"
#include "ipv6-end-point-demux.h"
#include "ipv4-end-point.h"
#include "ipv6-end-point.h"
#include "ipv4-l3-protocol.h"
#include "ipv6-l3-protocol.h"
#include "ipv6-routing-protocol.h"
#include "tcp-socket-factory-impl.h"
#include "tcp-socket-base.h"
#include "mptcp-socket-base.h"
#include "mptcp-subflow.h"
#include "tcp-option-mptcp.h"
#include "rtt-estimator.h"

#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <type_traits>
#include <memory>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TcpL4Protocol");

NS_OBJECT_ENSURE_REGISTERED (TcpL4Protocol);

//TcpL4Protocol stuff----------------------------------------------------------

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT                                   \
  if (m_node) { std::clog << Simulator::Now ().GetSeconds () << " [node " << m_node->GetId () << "] "; }

/* see http://www.iana.org/assignments/protocol-numbers */
const uint8_t TcpL4Protocol::PROT_NUMBER = 6;

TypeId
TcpL4Protocol::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpL4Protocol")
    .SetParent<IpL4Protocol> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpL4Protocol> ()
    .AddAttribute ("RttEstimatorType",
                   "Type of RttEstimator objects.",
                   TypeIdValue (RttMeanDeviation::GetTypeId ()),
                   MakeTypeIdAccessor (&TcpL4Protocol::m_rttTypeId),
                   MakeTypeIdChecker ())
    .AddAttribute ("SocketType",
                   "Socket type of TCP objects.",
                   TypeIdValue (TcpNewReno::GetTypeId ()),
                   MakeTypeIdAccessor (&TcpL4Protocol::m_congestionTypeId),
                   MakeTypeIdChecker ())
//    .AddAttribute ("EnableMpTcp", "Enable or disable MPTCP support",
//                   BooleanValue (false),
//                   MakeBooleanAccessor (&TcpL4Protocol::m_mptcpEnabled),
//                   MakeBooleanChecker ())
    .AddAttribute ("SocketList", "The list of sockets associated to this protocol.",
                   ObjectVectorValue (),
                   MakeObjectVectorAccessor (&TcpL4Protocol::m_sockets),
                   MakeObjectVectorChecker<TcpSocket> ())
    .AddAttribute ("OnNewSocket", "Callback invoked whenever a socket is created.",
                   CallbackValue (),
                   MakeCallbackAccessor (&TcpL4Protocol::m_onNewSocket),
                   MakeCallbackChecker ())
  ;
  return tid;
}

TcpL4Protocol::TcpL4Protocol ()
  :
    m_mptcpEnabled(false),
    m_endPoints (new Ipv4EndPointDemux ()),
    m_endPoints6 (new Ipv6EndPointDemux ())
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_LOG_LOGIC ("Made a TcpL4Protocol " << this);
}

TcpL4Protocol::~TcpL4Protocol ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

void
TcpL4Protocol::SetNode (Ptr<Node> node)
{
  m_node = node;
}

void
TcpL4Protocol::NotifyNewAggregate ()
{
  Ptr<Node> node = this->GetObject<Node> ();
  Ptr<Ipv4> ipv4 = this->GetObject<Ipv4> ();
  Ptr<Ipv6L3Protocol> ipv6 = node->GetObject<Ipv6L3Protocol> ();

  if (m_node == 0)
    {
      if ((node != 0) && (ipv4 != 0 || ipv6 != 0))
        {
          this->SetNode (node);
          Ptr<TcpSocketFactoryImpl> tcpFactory = CreateObject<TcpSocketFactoryImpl> ();
          tcpFactory->SetTcp (this);
          node->AggregateObject (tcpFactory);
        }
    }

  // We set at least one of our 2 down targets to the IPv4/IPv6 send
  // functions.  Since these functions have different prototypes, we
  // need to keep track of whether we are connected to an IPv4 or
  // IPv6 lower layer and call the appropriate one.

  if (ipv4 != 0 && m_downTarget.IsNull ())
    {
      ipv4->Insert (this);
      this->SetDownTarget (MakeCallback (&Ipv4::Send, ipv4));
    }
  if (ipv6 != 0 && m_downTarget6.IsNull ())
    {
      ipv6->Insert (this);
      this->SetDownTarget6 (MakeCallback (&Ipv6L3Protocol::Send, ipv6));
    }
  IpL4Protocol::NotifyNewAggregate ();
}

int
TcpL4Protocol::GetProtocolNumber (void) const
{
  return PROT_NUMBER;
}

void
TcpL4Protocol::DoDispose (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  m_sockets.clear ();

  if (m_endPoints != 0)
    {
      delete m_endPoints;
      m_endPoints = 0;
    }

  if (m_endPoints6 != 0)
    {
      delete m_endPoints6;
      m_endPoints6 = 0;
    }

  m_node = 0;
  m_downTarget.Nullify ();
  m_downTarget6.Nullify ();
  IpL4Protocol::DoDispose ();
}


Ptr<Socket>
TcpL4Protocol::CreateSocket (TypeId congestionTypeId, TypeId socketTypeId)
{
    ObjectFactory congestionAlgorithmFactory;
    congestionAlgorithmFactory.SetTypeId (congestionTypeId);
    Ptr<TcpCongestionOps> algo = congestionAlgorithmFactory.Create<TcpCongestionOps> ();

    return CreateSocket(algo, socketTypeId);
}

//// TODO create a ForkSocket Function ?
//Ptr<Socket>
//TcpL4Protocol::CreateSocket (TypeId congestionTypeId, TypeId socketTypeId)
//{
//}


Ptr<Socket>
TcpL4Protocol::CreateSocket (Ptr<TcpCongestionOps> algo, TypeId socketTypeId)
{
  NS_LOG_FUNCTION_NOARGS ();
  ObjectFactory rttFactory;
  ObjectFactory socketFactory;
  rttFactory.SetTypeId (m_rttTypeId);
  socketFactory.SetTypeId(socketTypeId);

  Ptr<RttEstimator> rtt = rttFactory.Create<RttEstimator> ();

  Ptr<TcpSocketBase> socket;

  // TODO allocate the max between children of tcpsocketbase ?
//  MpTcpSocketBase *addr = new MpTcpSocketBase;
//addr->~MpTcpSocketBase();
  NS_LOG_UNCOND(
                "sizeof(mtcp)=" << sizeof(MpTcpSocketBase)
                << "sizeof(aligned mtcp)=" << sizeof(std::aligned_storage<sizeof(MpTcpSocketBase)>::type)
                << " & sizeof(tcp) = "<< sizeof(TcpSocketBase)

                );

  NS_LOG_UNCOND( "socketTypeId=" << socketTypeId );
  /**
  This part is a hackish and creates memory leaks. The idea here is that when one creates a TcpSocketBase,
  there is the possibility that this socket may be replaced by an MpTcp Socket. In order for the application to
  see the same socket, we reallocate via the "placement new" technique
  **/
  if(socketTypeId == TcpSocketBase::GetTypeId())
  {
      //  char *addr = new char[ std::max(sizeof(MpTcpSocketBase), sizeof(TcpSocketBase))];
      char *addr = new char[sizeof(std::aligned_storage<sizeof(MpTcpSocketBase)>::type)];

      // now we should call the destructor ourself
      TcpSocketBase *temp = new (addr) TcpSocketBase();
      socket = CompleteConstruct (temp);
//      socket->Ref();
//      socket->Ref();
  }
  else
  {
    socket = socketFactory.Create<TcpSocketBase> ();
  }
  socket->SetNode (m_node);
  socket->SetTcp (this);
  socket->SetRtt (rtt);
  socket->InitLocalISN ();

  // TODO solve
  NS_LOG_DEBUG ("Test"  << algo);
  NS_LOG_DEBUG ("Setting CC with " << algo->GetName());
  NS_LOG_DEBUG ("Default CC =" << m_congestionTypeId.GetName());
  socket->SetCongestionControlAlgorithm (algo);

//  m_sockets.push_back (socket);
  AddSocket (socket);
  return socket;
}

Ptr<Socket>
TcpL4Protocol::CreateSocket (TypeId congestionTypeId)
{
    return CreateSocket (congestionTypeId, TcpSocketBase::GetTypeId());
}


Ptr<Socket>
TcpL4Protocol::CreateSocket (void)
{
  return CreateSocket (m_congestionTypeId);
}

Ipv4EndPoint *
TcpL4Protocol::Allocate (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  return m_endPoints->Allocate ();
}

Ipv4EndPoint *
TcpL4Protocol::Allocate (Ipv4Address address)
{
  NS_LOG_FUNCTION (this << address);
  return m_endPoints->Allocate (address);
}

Ipv4EndPoint *
TcpL4Protocol::Allocate (uint16_t port)
{
  NS_LOG_FUNCTION (this << port);
  return m_endPoints->Allocate (port);
}

Ipv4EndPoint *
TcpL4Protocol::Allocate (Ipv4Address address, uint16_t port)
{
  NS_LOG_FUNCTION (this << address << port);
//  NS_LOG_UNCOND("Matt should check if address belong to node before ?");
  // TODO map ipv4 to and NetDevice
  Ipv4EndPoint * endPoint = m_endPoints->Allocate (address, port);
//  if(endPoint) {
//    //!
//
//    Ptr<Ipv4> ipv4client = m_node->GetObject<Ipv4>();
//      for( uint32_t n =0; n < ipv4client->GetNInterfaces(); n++){
//        for( uint32_t a=0; a < ipv4client->GetNAddresses(n); a++){
//            NS_LOG_UNCOND( "Client addr " << n <<"/" << a << "=" << ipv4client->GetAddress(n,a));
//            if(address ==ipv4client->GetAddress(n,a).GetLocal()) {
//                NS_LOG_UNCOND("EUREKA same ip=" << address);
////                endPoint->BindToNetDevice(m_node->GetDevice(n));
//            }
//        }
//      }
//  }
  return endPoint;
}

Ipv4EndPoint *
TcpL4Protocol::Allocate (Ipv4Address localAddress, uint16_t localPort,
                         Ipv4Address peerAddress, uint16_t peerPort)
{
  NS_LOG_FUNCTION (this << localAddress << localPort << peerAddress << peerPort);
  return m_endPoints->Allocate (localAddress, localPort,
                                peerAddress, peerPort);
}

void
TcpL4Protocol::DeAllocate (Ipv4EndPoint *endPoint)
{
  NS_LOG_FUNCTION (this << endPoint);
  m_endPoints->DeAllocate (endPoint);
}

Ipv6EndPoint *
TcpL4Protocol::Allocate6 (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  return m_endPoints6->Allocate ();
}

Ipv6EndPoint *
TcpL4Protocol::Allocate6 (Ipv6Address address)
{
  NS_LOG_FUNCTION (this << address);
  return m_endPoints6->Allocate (address);
}

Ipv6EndPoint *
TcpL4Protocol::Allocate6 (uint16_t port)
{
  NS_LOG_FUNCTION (this << port);
  return m_endPoints6->Allocate (port);
}

Ipv6EndPoint *
TcpL4Protocol::Allocate6 (Ipv6Address address, uint16_t port)
{
  NS_LOG_FUNCTION (this << address << port);
  return m_endPoints6->Allocate (address, port);
}

Ipv6EndPoint *
TcpL4Protocol::Allocate6 (Ipv6Address localAddress, uint16_t localPort,
                          Ipv6Address peerAddress, uint16_t peerPort)
{
  NS_LOG_FUNCTION (this << localAddress << localPort << peerAddress << peerPort);
  return m_endPoints6->Allocate (localAddress, localPort,
                                 peerAddress, peerPort);
}

void
TcpL4Protocol::DeAllocate (Ipv6EndPoint *endPoint)
{
  NS_LOG_FUNCTION (this << endPoint);
  m_endPoints6->DeAllocate (endPoint);
}

void
TcpL4Protocol::ReceiveIcmp (Ipv4Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo,
                            Ipv4Address payloadSource,Ipv4Address payloadDestination,
                            const uint8_t payload[8])
{
  NS_LOG_FUNCTION (this << icmpSource << icmpTtl << icmpType << icmpCode << icmpInfo
                        << payloadSource << payloadDestination);
  uint16_t src, dst;
  src = payload[0] << 8;
  src |= payload[1];
  dst = payload[2] << 8;
  dst |= payload[3];

  Ipv4EndPoint *endPoint = m_endPoints->SimpleLookup (payloadSource, src, payloadDestination, dst);
  if (endPoint != 0)
    {
      endPoint->ForwardIcmp (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
  else
    {
      NS_LOG_DEBUG ("no endpoint found source=" << payloadSource <<
                    ", destination=" << payloadDestination <<
                    ", src=" << src << ", dst=" << dst);
    }
}

void
TcpL4Protocol::ReceiveIcmp (Ipv6Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo,
                            Ipv6Address payloadSource,Ipv6Address payloadDestination,
                            const uint8_t payload[8])
{
  NS_LOG_FUNCTION (this << icmpSource << icmpTtl << icmpType << icmpCode << icmpInfo
                        << payloadSource << payloadDestination);
  uint16_t src, dst;
  src = payload[0] << 8;
  src |= payload[1];
  dst = payload[2] << 8;
  dst |= payload[3];

  Ipv6EndPoint *endPoint = m_endPoints6->SimpleLookup (payloadSource, src, payloadDestination, dst);
  if (endPoint != 0)
    {
      endPoint->ForwardIcmp (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
  else
    {
      NS_LOG_DEBUG ("no endpoint found source=" << payloadSource <<
                    ", destination=" << payloadDestination <<
                    ", src=" << src << ", dst=" << dst);
    }
}

enum IpL4Protocol::RxStatus
TcpL4Protocol::PacketReceived (Ptr<Packet> packet, TcpHeader &incomingTcpHeader,
                               const Address &source, const Address &destination)
{

  if (Node::ChecksumEnabled ())
    {
      incomingTcpHeader.EnableChecksums ();
      incomingTcpHeader.InitializeChecksum (source, destination, PROT_NUMBER);
    }

  packet->PeekHeader (incomingTcpHeader);

  NS_LOG_LOGIC ("TcpL4Protocol " << this
                                 << " receiving seq " << incomingTcpHeader.GetSequenceNumber ()
                                 << " ack " << incomingTcpHeader.GetAckNumber ()
                                 << " flags "<< TcpHeader::FlagsToString (incomingTcpHeader.GetFlags ())
                                 << " data size " << packet->GetSize ());

  if (!incomingTcpHeader.IsChecksumOk ())
    {
      NS_LOG_INFO ("Bad checksum, dropping packet!");
      return IpL4Protocol::RX_CSUM_FAILED;
    }

  return IpL4Protocol::RX_OK;
}


/**
TODO return Meta & subflow ?
**/
//Ptr<MpTcpSubflow>
//TcpL4Protocol::UpgradeToMpTcpMetaSocket(Ptr<TcpSocketBase> socket)
//{
//
//
//  Ptr<MpTcpSubflow> master =  new MpTcpSubflow(*socket);
//  AddSocket(master);
////  MpTcpSocketBase* meta = new MpTcpSocketBase(*socket);
//
//  MpTcpSocketBase* meta = new (this) MpTcpSocketBase;
//
//  meta->AddSubflow(master);
//
//  return master;
//}


void
TcpL4Protocol::NoEndPointsFound (const TcpHeader &incomingHeader,
                                 const Address &incomingSAddr,
                                 const Address &incomingDAddr)
{
  if (!(incomingHeader.GetFlags () & TcpHeader::RST))
    {
      // build a RST packet and send
      Ptr<Packet> rstPacket = Create<Packet> ();
      TcpHeader outgoingTcpHeader;

      if (incomingHeader.GetFlags () & TcpHeader::ACK)
        {
          // ACK bit was set
          outgoingTcpHeader.SetFlags (TcpHeader::RST);
          outgoingTcpHeader.SetSequenceNumber (incomingHeader.GetAckNumber ());
        }
      else
        {
          outgoingTcpHeader.SetFlags (TcpHeader::RST | TcpHeader::ACK);
          outgoingTcpHeader.SetSequenceNumber (SequenceNumber32 (0));
          outgoingTcpHeader.SetAckNumber (incomingHeader.GetSequenceNumber () +
                                          SequenceNumber32 (1));
        }

      // Remember that parameters refer to the incoming packet; in reply,
      // we need to swap src/dst

      outgoingTcpHeader.SetSourcePort (incomingHeader.GetDestinationPort ());
      outgoingTcpHeader.SetDestinationPort (incomingHeader.GetSourcePort ());

      SendPacket (rstPacket, outgoingTcpHeader, incomingDAddr, incomingSAddr);
    }
}

Ptr<TcpSocket>
TcpL4Protocol::LookupMpTcpToken (uint32_t token)
{


  //! We should find the token
    NS_LOG_INFO("Looking for token=" << token
        << " among " << m_sockets.size() << " sockets "
        );

    /* We go through all the metas to find one with the correct token */
    for(std::vector<Ptr<TcpSocket> >::iterator it = m_sockets.begin(), last(m_sockets.end());
      it != last;
      it++
     )
    {
          Ptr<TcpSocket> sock = *it;
          Ptr<MpTcpSocketBase> meta = DynamicCast<MpTcpSocketBase>( sock );
          Address addr;
          (*it)->GetSockName(addr);
          NS_LOG_DEBUG("Socket : " << sock
                << " socket of type=" << sock->GetInstanceTypeId());
          if(!meta)
          {
            NS_LOG_DEBUG("Conversion failed: " << sock << " is not an mptcp socket");
            continue;
          }

          NS_LOG_DEBUG("Conversion succeeded: " << sock << " is an mptcp socket. Comparing "
                        "meta->GetLocalToken()=" << meta->GetLocalToken() << " and token="<<  token);
          if(meta->GetLocalToken() == token)
          {
              NS_LOG_DEBUG("Found match " << &meta);
              return meta;

    //        NS_LOG_DEBUG("Token " << meta->GetToken() << " differ from MP_JOIN token " << join->GetPeerToken());
    //        continue;
          }


    }

    return 0;
}


enum IpL4Protocol::RxStatus
TcpL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv4Header const &incomingIpHeader,
                        Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << packet << incomingIpHeader << incomingInterface);

  TcpHeader incomingTcpHeader;
  IpL4Protocol::RxStatus checksumControl;

  checksumControl = PacketReceived (packet, incomingTcpHeader,
                                    incomingIpHeader.GetSource (),
                                    incomingIpHeader.GetDestination ());

  if (checksumControl != IpL4Protocol::RX_OK)
    {
      return checksumControl;
    }

  NS_LOG_LOGIC ("TcpL4Protocol " << this << " received a packet");

  Ipv4EndPointDemux::EndPoints endPoints;
  endPoints = m_endPoints->Lookup (incomingIpHeader.GetDestination (),
                                   incomingTcpHeader.GetDestinationPort (),
                                   incomingIpHeader.GetSource (),
                                   incomingTcpHeader.GetSourcePort (),
                                   incomingInterface);


  /**
  TODO clean this part, maybe TcpL4protocol should be reworked a bit
  **/
  if (endPoints.empty())
  {
      NS_LOG_LOGIC ("No Ipv4 endpoints matched on TcpL4Protocol, "
                    "checking if packet is a MP_JOIN request:" << incomingIpHeader);

    // MPTCP related modification----------------------------
    // Extract MPTCP options if there is any
    Ptr<const TcpOptionMpTcpJoin> join;
    Ptr<MpTcpSocketBase> meta;

    // If it is a SYN packet with an MP_JOIN option
    if( (incomingTcpHeader.GetFlags() & TcpHeader::SYN)
        && GetTcpOption(incomingTcpHeader, join)
        && join->GetMode() == TcpOptionMpTcpJoin::Syn
       )
    {
        NS_LOG_DEBUG("This is indeed a MP_JOIN");

        meta = DynamicCast<MpTcpSocketBase>(LookupMpTcpToken(join->GetPeerToken()));
        if(meta)
        {

            NS_LOG_LOGIC ("Found meta " << meta << " matching MP_JOIN token=" << join->GetPeerToken());

            Ipv4EndPoint *endP =  meta->NewSubflowRequest(
                  packet,
                  incomingTcpHeader,
                  InetSocketAddress(incomingIpHeader.GetSource(), incomingTcpHeader.GetSourcePort() ),
                  InetSocketAddress(incomingIpHeader.GetDestination(), incomingTcpHeader.GetDestinationPort() ) ,
                  join
                  );

            NS_LOG_DEBUG("value of endP=" << endP);

            // TODO check that it sends a RST otherwise
            if(endP)
            {
              NS_LOG_DEBUG("subflow endpoint pushed in vector");
              endPoints.push_back(endP);

              // if we don't break here, then it will infintely loop, each time pushing a new SocketBase with a valid token
              //return IpL4Protocol::RX_OK;
            }
        }
    }

//        NS_ASSERT_MSG(endPoints.size () == 1, "Demux returned more or less than one endpoint");
//        (*endPoints.begin())->ForwardUp(packet, ipHeader, tcpHeader.GetSourcePort(), incomingInterface);

  //    }
  //    else {
  //      NS_LOG_DEBUG("Ignore MP_JOIN with state " << join->GetState());
  //    }


  }

  if (endPoints.empty ())
    {
      if (this->GetObject<Ipv6L3Protocol> () != 0)
        {
          NS_LOG_LOGIC ("  No Ipv4 endpoints matched on TcpL4Protocol, trying Ipv6 " << this);
          Ptr<Ipv6Interface> fakeInterface;
          Ipv6Header ipv6Header;
          Ipv6Address src, dst;

          src = Ipv6Address::MakeIpv4MappedAddress (incomingIpHeader.GetSource ());
          dst = Ipv6Address::MakeIpv4MappedAddress (incomingIpHeader.GetDestination ());
          ipv6Header.SetSourceAddress (src);
          ipv6Header.SetDestinationAddress (dst);
          return (this->Receive (packet, ipv6Header, fakeInterface));
        }

      NS_LOG_LOGIC ("No endpoints matched on TcpL4Protocol "<< this <<
                    " destination IP: " << incomingIpHeader.GetDestination () <<
                    " destination port: "<< incomingTcpHeader.GetDestinationPort () <<
                    " source IP: " << incomingIpHeader.GetSource () <<
                    " source port: "<< incomingTcpHeader.GetSourcePort ());

      NoEndPointsFound (incomingTcpHeader, incomingIpHeader.GetSource (),
                        incomingIpHeader.GetDestination ());

      return IpL4Protocol::RX_ENDPOINT_CLOSED;

    }

  NS_ASSERT_MSG (endPoints.size () == 1, "Demux returned more than one endpoint");
  NS_LOG_LOGIC ("TcpL4Protocol " << this << " forwarding up to endpoint/socket " << (*endPoints.begin ()));

  (*endPoints.begin ())->ForwardUp (packet, incomingIpHeader,
                                    incomingTcpHeader.GetSourcePort (),
                                    incomingInterface);

  return IpL4Protocol::RX_OK;
}

enum IpL4Protocol::RxStatus
TcpL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv6Header const &incomingIpHeader,
                        Ptr<Ipv6Interface> interface)
{
  NS_LOG_FUNCTION (this << packet << incomingIpHeader.GetSourceAddress () <<
                   incomingIpHeader.GetDestinationAddress ());

  TcpHeader incomingTcpHeader;
  IpL4Protocol::RxStatus checksumControl;

  // If we are receving a v4-mapped packet, we will re-calculate the TCP checksum
  // Is it worth checking every received "v6" packet to see if it is v4-mapped in
  // order to avoid re-calculating TCP checksums for v4-mapped packets?

  checksumControl = PacketReceived (packet, incomingTcpHeader,
                                    incomingIpHeader.GetSourceAddress (),
                                    incomingIpHeader.GetDestinationAddress ());

  if (checksumControl != IpL4Protocol::RX_OK)
    {
      return checksumControl;
    }

  NS_LOG_LOGIC ("TcpL4Protocol " << this << " received a packet");
  Ipv6EndPointDemux::EndPoints endPoints =
    m_endPoints6->Lookup (incomingIpHeader.GetDestinationAddress (),
                          incomingTcpHeader.GetDestinationPort (),
                          incomingIpHeader.GetSourceAddress (),
                          incomingTcpHeader.GetSourcePort (), interface);
  if (endPoints.empty ())
    {
      NS_LOG_LOGIC ("No endpoints matched on TcpL4Protocol "<< this <<
                    " destination IP: " << incomingIpHeader.GetDestinationAddress () <<
                    " destination port: "<< incomingTcpHeader.GetDestinationPort () <<
                    " source IP: " << incomingIpHeader.GetSourceAddress () <<
                    " source port: "<< incomingTcpHeader.GetSourcePort ());

      NoEndPointsFound (incomingTcpHeader, incomingIpHeader.GetSourceAddress (),
                        incomingIpHeader.GetDestinationAddress ());

      return IpL4Protocol::RX_ENDPOINT_CLOSED;
    }

  NS_ASSERT_MSG (endPoints.size () == 1, "Demux returned more than one endpoint");
  NS_LOG_LOGIC ("TcpL4Protocol " << this << " forwarding up to endpoint/socket");

  (*endPoints.begin ())->ForwardUp (packet, incomingIpHeader,
                                    incomingTcpHeader.GetSourcePort (), interface);

  return IpL4Protocol::RX_OK;
}

void
TcpL4Protocol::SendPacketV4 (Ptr<Packet> packet, const TcpHeader &outgoing,
                             const Ipv4Address &saddr, const Ipv4Address &daddr,
                             Ptr<NetDevice> oif) const
{
  NS_LOG_LOGIC ("TcpL4Protocol " << this
                                 << " sending seq " << outgoing.GetSequenceNumber ()
                                 << " ack " << outgoing.GetAckNumber ()
                                 << " flags " << TcpHeader::FlagsToString (outgoing.GetFlags ())
                                 << " data size " << packet->GetSize ());
  NS_LOG_FUNCTION (this << packet << saddr << daddr << oif);
  // XXX outgoingHeader cannot be logged

  TcpHeader outgoingHeader = outgoing;
  /** \todo UrgentPointer */
  /* outgoingHeader.SetUrgentPointer (0); */
  if (Node::ChecksumEnabled ())
    {
      outgoingHeader.EnableChecksums ();
    }
  outgoingHeader.InitializeChecksum (saddr, daddr, PROT_NUMBER);

  packet->AddHeader (outgoingHeader);

  Ptr<Ipv4> ipv4 =
    m_node->GetObject<Ipv4> ();
  if (ipv4 != 0)
    {
      Ipv4Header header;
      header.SetSource (saddr);
      header.SetDestination (daddr);
      header.SetProtocol (PROT_NUMBER);
      Socket::SocketErrno errno_;
      Ptr<Ipv4Route> route;
      if (ipv4->GetRoutingProtocol () != 0)
        {
          route = ipv4->GetRoutingProtocol ()->RouteOutput (packet, header, oif, errno_);
        }
      else
        {
          NS_LOG_ERROR ("No IPV4 Routing Protocol");
          route = 0;
        }
      m_downTarget (packet, saddr, daddr, PROT_NUMBER, route);
    }
  else
    {
      NS_FATAL_ERROR ("Trying to use Tcp on a node without an Ipv4 interface");
    }
}

void
TcpL4Protocol::SendPacketV6 (Ptr<Packet> packet, const TcpHeader &outgoing,
                             const Ipv6Address &saddr, const Ipv6Address &daddr,
                             Ptr<NetDevice> oif) const
{
  NS_LOG_LOGIC ("TcpL4Protocol " << this
                                 << " sending seq " << outgoing.GetSequenceNumber ()
                                 << " ack " << outgoing.GetAckNumber ()
                                 << " flags " << TcpHeader::FlagsToString (outgoing.GetFlags ())
                                 << " data size " << packet->GetSize ());
  NS_LOG_FUNCTION (this << packet << saddr << daddr << oif);
  // XXX outgoingHeader cannot be logged

  if (daddr.IsIpv4MappedAddress ())
    {
      return (SendPacket (packet, outgoing, saddr.GetIpv4MappedAddress (), daddr.GetIpv4MappedAddress (), oif));
    }
  TcpHeader outgoingHeader = outgoing;
  /** \todo UrgentPointer */
  /* outgoingHeader.SetUrgentPointer (0); */
  if (Node::ChecksumEnabled ())
    {
      outgoingHeader.EnableChecksums ();
    }
  outgoingHeader.InitializeChecksum (saddr, daddr, PROT_NUMBER);

  packet->AddHeader (outgoingHeader);

  Ptr<Ipv6L3Protocol> ipv6 = m_node->GetObject<Ipv6L3Protocol> ();
  if (ipv6 != 0)
    {
      Ipv6Header header;
      header.SetSourceAddress (saddr);
      header.SetDestinationAddress (daddr);
      header.SetNextHeader (PROT_NUMBER);
      Socket::SocketErrno errno_;
      Ptr<Ipv6Route> route;
      if (ipv6->GetRoutingProtocol () != 0)
        {
          route = ipv6->GetRoutingProtocol ()->RouteOutput (packet, header, oif, errno_);
        }
      else
        {
          NS_LOG_ERROR ("No IPV6 Routing Protocol");
          route = 0;
        }
      m_downTarget6 (packet, saddr, daddr, PROT_NUMBER, route);
    }
  else
    {
      NS_FATAL_ERROR ("Trying to use Tcp on a node without an Ipv6 interface");
    }
}

void
TcpL4Protocol::SendPacket (Ptr<Packet> pkt, const TcpHeader &outgoing,
                           const Address &saddr, const Address &daddr,
                           Ptr<NetDevice> oif) const
{
  if (Ipv4Address::IsMatchingType (saddr))
    {
      NS_ASSERT (Ipv4Address::IsMatchingType (daddr));

      SendPacketV4 (pkt, outgoing, Ipv4Address::ConvertFrom (saddr),
                    Ipv4Address::ConvertFrom (daddr), oif);

      return;
    }
  else if (Ipv6Address::IsMatchingType (saddr))
    {
      NS_ASSERT (Ipv6Address::IsMatchingType (daddr));

      SendPacketV6 (pkt, outgoing, Ipv6Address::ConvertFrom (saddr),
                    Ipv6Address::ConvertFrom (daddr), oif);

      return;
    }
  else if (InetSocketAddress::IsMatchingType (saddr))
    {
      InetSocketAddress s = InetSocketAddress::ConvertFrom (saddr);
      InetSocketAddress d = InetSocketAddress::ConvertFrom (daddr);

      SendPacketV4 (pkt, outgoing, s.GetIpv4 (), d.GetIpv4 (), oif);

      return;
    }
  else if (Inet6SocketAddress::IsMatchingType (saddr))
    {
      Inet6SocketAddress s = Inet6SocketAddress::ConvertFrom (saddr);
      Inet6SocketAddress d = Inet6SocketAddress::ConvertFrom (daddr);

      SendPacketV6 (pkt, outgoing, s.GetIpv6 (), d.GetIpv6 (), oif);

      return;
    }

  NS_FATAL_ERROR ("Trying to send a packet without IP addresses");
}

void
TcpL4Protocol::DumpSockets () const
{
    NS_LOG_UNCOND ("== Dumping sockets ==");
    for(std::vector<Ptr<TcpSocket> >::const_iterator it = m_sockets.cbegin(), last(m_sockets.cend());
      it != last;
      it++
     )
    {
        Ptr<TcpSocket> sock = *it;
        NS_LOG_DEBUG("Socket : " << sock << " socket of type=" << sock->GetInstanceTypeId());
    }
    NS_LOG_UNCOND ("== end of dump ==");
}


bool
TcpL4Protocol::NotifyNewSocket (Ptr<TcpSocket> socket)
{
  if(!m_onNewSocket.IsNull())
  {
    NS_LOG_DEBUG ("Calling m_onNewSocket");
    m_onNewSocket (socket);
  }
  return true;
}

/**
TODO we should check MPTCP token is not registered twice
**/
bool
TcpL4Protocol::AddSocket (Ptr<TcpSocket> socket)
{
//  std::vector<Ptr<TcpSocketBase> >::iterator it = m_sockets.begin ();

//  while (it != m_sockets.end ())
//    {
//      if (*it == socket)
//        {
//          return;
//        }
//
//      ++it;
//    }
  NS_LOG_FUNCTION(socket);
  // TODO remove afterwards
  DumpSockets();




  std::vector<Ptr<TcpSocket> >::iterator it = std::find(m_sockets.begin(), m_sockets.end(), socket);
  if (it == m_sockets.end())
  {
    m_sockets.push_back (socket);
    NotifyNewSocket (socket);
    return true;
  }
  return false;
}

bool
TcpL4Protocol::RemoveSocket (Ptr<TcpSocket> socket)
{
  std::vector<Ptr<TcpSocket> >::iterator it = m_sockets.begin ();


  while (it != m_sockets.end ())
    {
      if (*it == socket)
        {
          m_sockets.erase (it);
          return true;
        }

      ++it;
    }

  return false;
//  std::remove(m_sockets.begin(), m_sockets.end(), socket);
}

void
TcpL4Protocol::SetDownTarget (IpL4Protocol::DownTargetCallback callback)
{
  m_downTarget = callback;
}

IpL4Protocol::DownTargetCallback
TcpL4Protocol::GetDownTarget (void) const
{
  return m_downTarget;
}

void
TcpL4Protocol::SetDownTarget6 (IpL4Protocol::DownTargetCallback6 callback)
{
  m_downTarget6 = callback;
}

IpL4Protocol::DownTargetCallback6
TcpL4Protocol::GetDownTarget6 (void) const
{
  return m_downTarget6;
}

} // namespace ns3

