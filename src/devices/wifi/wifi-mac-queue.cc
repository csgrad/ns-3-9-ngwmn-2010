/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2005, 2009 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
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
 * Author: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 * Author: Mirko Banchi <mk.banchi@gmail.com>
 */
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "wifi-phy.h"
#include "wifi-mac-queue.h"
#include "wifi-channel.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/mobility-model.h"

#include "wifi-mac-queue.h"
#include "qos-blocked-destinations.h"
#include "ns3/random-variable.h"
#include "ns3/udp-server.h"

/* Constants for Mixed Bias Mac Layer */
#define MB			0
#define ADAPTIVE		0
#define A			0.5
#define B1			2.0
#define B2			5.0

/* Used to compute the hops */
#define	SEPARATIONDISTANCE	100

/* Used to define how often the tabu is reconfigured */
#define PACKETRESET 5			//how often we make a tabu move
#define TABURESET	50			//how often we have a chance of reset to best move
#define TABULIFE	5			//time in seconds a move a tabu
using namespace std;

namespace ns3 {

Solution::Solution() :
  ALPHA(0.5),
  BETA1(2),
  BETA2(5),
  utility(0)
{}

NS_OBJECT_ENSURE_REGISTERED (WifiMacQueue);

WifiMacQueue::Item::Item (Ptr<const Packet> packet, 
                          const WifiMacHeader &hdr, 
                          Time tstamp)
  : packet (packet), hdr (hdr), tstamp (tstamp)
{}

TypeId 
WifiMacQueue::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::WifiMacQueue")
    .SetParent<Object> ()
    .AddConstructor<WifiMacQueue> ()
    .AddAttribute ("MaxPacketNumber", "If a packet arrives when there are already this number of packets, it is dropped.",
                   UintegerValue (400),
                   MakeUintegerAccessor (&WifiMacQueue::m_maxSize),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("MaxDelay", "If a packet stays longer than this delay in the queue, it is dropped.",
                   TimeValue (Seconds (10.0)),
                   MakeTimeAccessor (&WifiMacQueue::m_maxDelay),
                   MakeTimeChecker ())
    ;
  return tid;
}

WifiMacQueue::WifiMacQueue ()
  : m_size (0)
{
	distance = -1;
	packetCount = 0;
	iterationCount = 0;
}

WifiMacQueue::~WifiMacQueue ()
{
	//std::cout << "BEST SOLUTION - A: " << best.ALPHA << " B1: " << best.BETA1 << " B2: " << best.BETA2 << std::endl;	
  Flush ();
}

void 
WifiMacQueue::SetMaxSize (uint32_t maxSize)
{
  m_maxSize = maxSize;
}

void
WifiMacQueue::SetMaxDelay (Time delay)
{
  m_maxDelay = delay;
}

uint32_t 
WifiMacQueue::GetMaxSize (void) const
{
  return m_maxSize;
}

Time 
WifiMacQueue::GetMaxDelay (void) const
{
  return m_maxDelay;
}

void 
WifiMacQueue::Enqueue (Ptr<const Packet> packet, const WifiMacHeader &hdr)
{
  //std::cout << "MAXDELAY: " << m_maxDelay << std::endl;
  Cleanup ();
  if (m_size == m_maxSize) 
  {
     return;
  }
   
  //mixed bias updates: Jason Ernst, University of Guelph
  //note required packet to be upated to support Delayed() and Delay() functions
  if(MB)
  {
	  if(distance == -1)
	    ComputeDistance(hdr);
	  
	  //std::cout << "Distance: " << distance << std::endl;
      int hops = distance / (double)SEPARATIONDISTANCE; 	//convert distance to hops
      double R;
	  
	  //normal mb
	  if(!ADAPTIVE)
	  {
		R = (A / pow(hops,B1)) + ((1-A) / (pow(hops,B2)));
		R = R * 5;
		if(hops == 1)
			R = 0.95;
		//std::cout << "HOPS: " << hops << " R: " << R << std::endl;	
		UniformVariable u(0,1);
	    double prob = u.GetValue();
	    
	    //this packet will be delayed if prob < R and it hasnt already been delayed
	    Time now;
	    Time delay = Seconds(0.5); //may be adjusted in adaptive version as well
	    Ptr<Packet> p = packet->Copy(); //copy the packet since the const forces us to
	    if(prob > R && !p->Delayed())
	    {
			p->Delay();
			now = Simulator::Now () + delay;
		}
		else
			now = Simulator::Now();
		
		m_queue.push_back (Item (p, hdr, now));
		m_size++;
	  }
	  //adaptive mb
	  else
	  {
		R = (current.ALPHA / pow(hops,current.BETA1)) + ((1-current.ALPHA) / (pow(hops,current.BETA2)));
		R = R * 5;
		if(hops == 1)
			R = 0.95;
		//std::cout << "HOPS: " << hops << " R: " << R << std::endl;	
		UniformVariable u(0,1);
	    double prob = u.GetValue();
	    
	    //std::cout << "CURRENT SOLUTION - A: " << current.ALPHA << " B1: " << current.BETA1 << " B2: " << current.BETA2 << std::endl;
		//std::cout << "R: " << R << std::endl;
	    
	    //this packet will be delayed if prob < R and it hasnt already been delayed
	    Time now;
	    Time delay = Seconds(0.5); //may be adjusted in adaptive version as well
	    Ptr<Packet> p = packet->Copy(); //copy the packet since the const forces us to
	    if(prob > R && !p->Delayed())
	    {
			p->Delay();
			now = Simulator::Now () + delay;
			packetCount++;
			//std::cout << "pcount: " << packetCount << std::endl;
			if(packetCount > PACKETRESET)
			{
				//std::cout << "BEST SOLUTION - A: " << best.ALPHA << " B1: " << best.BETA1 << " B2: " << best.BETA2 << std::endl;	
				//std::cout << "CURRENT SOLUTION - A: " << current.ALPHA << " B1: " << current.BETA1 << " B2: " << current.BETA2 << std::endl;
				//remove expired tabus
				unsigned int x;
				for(x = 0; x<tabuList.size();x++)
				{
					if(tabuList[x].expiry < Now())
					{
						tabuList.erase(tabuList.begin()+x);
						x=0;
					}
				}			
				packetCount = 0;
				iterationCount++;
				
				//Strategy:
				//- start with low A
				//- increase A (emphasize weak bias)
				//- start with high B1, B2, decrease
				Ptr<Channel> channel = m_phy->GetChannel();
				Ptr<NetDevice> device = channel->GetDevice(0); //assume the GW is the first device
				Ptr<Node> destination = device->GetNode();
				Ptr<Application> app = destination->GetApplication(0);
				Ptr<UdpServer> server = app->GetObject<UdpServer> ();
				
				double received = 0;
				double delay = 0;
				double pdr = 0;
				if(server != NULL)
				{
					//std::cout << "NOT NULL" << std::endl;
					received = (double)server->GetReceived ();
					if(received == 0)
						received = 1;
					delay = (server->GetTotalDelay().GetSeconds())/received;
					pdr = (double)server->GetReceived () / (received + (double)server->GetLost ());
				}
				else
					std::cout << "CANNOT TRACK RECEIVED PACKETS, CHECK WIFIMACQUEUE TO SEE IF THE DEVICE # MATCHES YOUR NODE WITH UDP SERVER INSTALLED" << std::endl;
							
				//avoid div by 0, if 0 delay, likely no packets delivered
				if(delay == 0)
					delay = 100000;	
				double utility = 1/delay + pdr;
				current.utility = utility;
				
				//std::cout << " util: " << utility << std::endl;
					
				//see if we have a new best configuration
				if(utility > best.utility)
				{
					best.utility = utility;
					best.ALPHA = current.ALPHA;
					best.BETA1 = current.BETA1;
					best.BETA2 = current.BETA2;
				}
				
				//search for potential new configuration
				Solution potential = GetPotentialSolution();
				while(IsTabu(potential))
					potential = GetPotentialSolution();
				
				current.ALPHA = potential.ALPHA;
				current.BETA1 = potential.BETA1;
				current.BETA2 = potential.BETA2;
				current.utility = 0;
				
				Tabu newtabu;
				newtabu.expiry = Now() + Time(Seconds(TABULIFE));
				newtabu.ALPHA = current.ALPHA;
				newtabu.BETA1 = current.BETA1;
				newtabu.BETA2 = current.BETA2;				
				tabuList.push_back(newtabu);
				
				//Aspiration Criteria:
				//- certain probability of restarting back at starting values
				if(iterationCount > TABURESET)
				{
					double prob2 = u.GetValue();
					if(prob2 < .5)
					{
						current.ALPHA = best.ALPHA;
						current.BETA1 = best.BETA1;
						current.BETA2 = best.BETA2;
						current.utility = best.utility;
						iterationCount = 0;
					}
				}
			}
		}
		else
			now = Simulator::Now();
		
		m_queue.push_back (Item (p, hdr, now));
		m_size++;
	} // end of adaptive MB
  }
  else
  {
	packetCount++;
	iterationCount++;
	Time now = Simulator::Now ();
	m_queue.push_back (Item (packet, hdr, now));
	m_size++;
  }
}

//added by Jason
void WifiMacQueue::SetPhy(Ptr<WifiPhy> phy)
{
	m_phy = phy;
}

void WifiMacQueue::ComputeDistance(const WifiMacHeader &hdr)
{
	//determine distance (move all into a compute function which is only computed if distance is -1)
	Mac48Address to = hdr.GetAddr1();
    Mac48Address from = hdr.GetAddr2();
	Ptr<WifiChannel> channel = m_phy->GetChannel();
    Ptr<Node> source;
    Ptr<NetDevice> gwDevice = channel->GetDevice(0);
	Ptr<Node> destination = gwDevice->GetNode();
    
    for(uint32_t x=0;x<channel->GetNDevices();x++)
	{
		Ptr<NetDevice> device = channel->GetDevice(x);
		if(Mac48Address::ConvertFrom(device->GetAddress()) == from)
		{
			source = device->GetNode();
			break;
		}
    }
    
    Ptr<MobilityModel> msource = source->GetObject<MobilityModel> ();
	Ptr<MobilityModel> mdest = destination->GetObject<MobilityModel> ();
	distance = msource->GetDistanceFrom(mdest);
}

void
WifiMacQueue::Cleanup (void)
{
  if (m_queue.empty ()) 
    {
      return;
    }

  Time now = Simulator::Now ();
  uint32_t n = 0;
  for (PacketQueueI i = m_queue.begin (); i != m_queue.end ();) 
    {
      if (i->tstamp + m_maxDelay > now) 
        {
          i++;
        }
      else
        {
          i = m_queue.erase (i);
          n++;
        }
    }
  m_size -= n;
}

Ptr<const Packet>
WifiMacQueue::Dequeue (WifiMacHeader *hdr)
{
  Cleanup ();
  if (!m_queue.empty ()) 
    {
      Item i = m_queue.front ();
      m_queue.pop_front ();
      m_size--;
      *hdr = i.hdr;
      return i.packet;
    }
  return 0;
}

Ptr<const Packet>
WifiMacQueue::Peek (WifiMacHeader *hdr)
{
  Cleanup ();
  if (!m_queue.empty ()) 
    {
      Item i = m_queue.front ();
      *hdr = i.hdr;
      return i.packet;
    }
  return 0;
}

Ptr<const Packet>
WifiMacQueue::DequeueByTidAndAddress (WifiMacHeader *hdr, uint8_t tid, 
                                      WifiMacHeader::AddressType type, Mac48Address dest)
{
  Cleanup ();
  Ptr<const Packet> packet = 0;
  if (!m_queue.empty ())
    {
      PacketQueueI it;
      NS_ASSERT (type <= 4);
      for (it = m_queue.begin (); it != m_queue.end (); ++it)
        {
          if (it->hdr.IsQosData ())
            {
              if (GetAddressForPacket (type, it) == dest &&
                  it->hdr.GetQosTid () == tid)
                {
                  packet = it->packet;
                  *hdr = it->hdr;
                  m_queue.erase (it);
                  m_size--;
                  break;
                }
            }
        }
    }
  return packet;
}

Ptr<const Packet>
WifiMacQueue::PeekByTidAndAddress (WifiMacHeader *hdr, uint8_t tid, 
                                   WifiMacHeader::AddressType type, Mac48Address dest)
{
  Cleanup ();
  if (!m_queue.empty ())
    {
      PacketQueueI it;
      NS_ASSERT (type <= 4);
      for (it = m_queue.begin (); it != m_queue.end (); ++it)
        {
          if (it->hdr.IsQosData ())
            {
              if (GetAddressForPacket (type, it) == dest &&
                  it->hdr.GetQosTid () == tid)
                {
                  *hdr = it->hdr;
                  return it->packet;
                }
            }
        }
    }
  return 0;
}

bool
WifiMacQueue::IsEmpty (void)
{
  Cleanup ();
  return m_queue.empty ();
}

/*
 * Returns true if a given solution is already in the tabu list
 */
bool WifiMacQueue::IsTabu(Solution potential)
{
	unsigned int c;
	for(c = 0; c < tabuList.size(); c++)
	{
		if(potential.ALPHA == tabuList[c].ALPHA && potential.BETA1 == tabuList[c].BETA1 && potential.BETA2 == tabuList[c].BETA2)
		{
			return true;
		}
	}	
	return false;
}

uint32_t
WifiMacQueue::GetSize (void)
{
  return m_size;
}

void
WifiMacQueue::Flush (void)
{
  m_queue.erase (m_queue.begin (), m_queue.end ());
  m_size = 0;
}

Mac48Address
WifiMacQueue::GetAddressForPacket (enum WifiMacHeader::AddressType type, PacketQueueI it)
{
  if (type == WifiMacHeader::ADDR1)
    {
      return it->hdr.GetAddr1 ();
    }
  if (type == WifiMacHeader::ADDR2)
    {
      return it->hdr.GetAddr2 ();
    }
  if (type == WifiMacHeader::ADDR3)
    {
      return it->hdr.GetAddr3 ();
    }
  return 0;
}

bool
WifiMacQueue::Remove (Ptr<const Packet> packet)
{
  PacketQueueI it = m_queue.begin ();
  for (; it != m_queue.end (); it++)
    {
      if (it->packet == packet)
        {
          m_queue.erase (it);
          m_size--;
          return true;
        }
    }
  return false;
}

void
WifiMacQueue::PushFront (Ptr<const Packet> packet, const WifiMacHeader &hdr)
{
  Cleanup ();
  if (m_size == m_maxSize)
    {
      return;
    }
  Time now = Simulator::Now ();
  m_queue.push_front (Item (packet, hdr, now));
  m_size++;
}

uint32_t
WifiMacQueue::GetNPacketsByTidAndAddress (uint8_t tid, WifiMacHeader::AddressType type,
                                          Mac48Address addr)
{
  Cleanup ();
  uint32_t nPackets = 0;
  if (!m_queue.empty ())
    {
      PacketQueueI it;
      NS_ASSERT (type <= 4);
      for (it = m_queue.begin (); it != m_queue.end (); it++)
        {
          if (GetAddressForPacket (type, it) == addr)
            {
              if (it->hdr.IsQosData () && it->hdr.GetQosTid () == tid)
                {
                  nPackets++;
                }
            }
        }
    }
  return nPackets;
}

Solution WifiMacQueue::GetPotentialSolution()
{
	Solution potential;
	UniformVariable u(0,1);
	double choice;
	
	//alpha
	choice = u.GetValue();
	//up
	if(choice <= 0.45)
	{
		if(current.ALPHA + 0.1 < 1)
			potential.ALPHA = current.ALPHA + 0.1;	
	}
	//down
	else if (choice <= 0.9)
	{
		if(current.ALPHA - 0.1 > 0)
			potential.ALPHA = current.ALPHA - 0.1;
	}
	//rand
	else
	{
		double rand = u.GetValue();
		potential.ALPHA = rand;
	}
	
	//beta1
	choice = u.GetValue();
	if(choice <= 0.45)
	{
		if(current.BETA1 + 0.5 < 7.5)
			potential.BETA1 = current.BETA1 + 0.5;
	}
	else if(choice <= 0.9)
	{
		if(current.BETA1 - 0.5 > 0)
			potential.BETA1 = current.BETA1 - 0.5;
	}
	else
	{
		int rand = (int)(u.GetValue() * 10);
		potential.BETA1 = rand;
	}
	
	//beta2
	choice = u.GetValue();
	if(choice <= 0.45)
	{
		if(current.BETA2 + 0.5 < 7.5)
			potential.BETA2 = current.BETA2 + 0.5;
	}
	else if(choice <= 0.9)
	{
		if(current.BETA2 - 0.5 > 0)
			potential.BETA2 = current.BETA2 - 0.5;
	}
	else
	{
		int rand = (int)(u.GetValue() * 10);
		potential.BETA2 = rand;
	}
			
	return potential;
}

/*
 * Uses the current solution to generate the next potential solution
 * (Don't need to use any parameters since everything we need is within WifiMacQueue)
 *
Solution WifiMacQueue::GetPotentialSolution()
{
	Solution potential;
	UniformVariable u(0,1);
	//determine which variable to change subject to some restrictions
	double prob2 = u.GetValue();
	
	//adust alpha up
	if(prob2 < 0.3)
	{
		if(current.ALPHA + 0.1 < 1)
		{
			potential.ALPHA += 0.1;
			potential.BETA1 = current.BETA1;
			potential.BETA2 = current.BETA2;
		}
	}
	//adjust beta1 down
	else if(prob2 < 0.6)
	{
		if(current.BETA1 - 0.1 > 0.5)
		{
			potential.BETA1 -= 0.1;
			potential.ALPHA = current.ALPHA;
			potential.BETA2 = current.BETA2;
		}
	}
	
	//adjust beta2 down
	else if(prob2 < 0.9)
	{
		if(current.BETA2 - 0.1 > 0.5 && current.BETA2 - 0.1 > current.BETA1)
		{
			potential.BETA2 -= 0.1;
			potential.ALPHA = current.ALPHA;
			potential.BETA1 = current.BETA1;
		}
	}
	
	//assign random value
	else
	{
		double choice = u.GetValue();
		if(choice <= .7)
		{
			double prob3 = u.GetValue();
			potential.ALPHA = prob3;
		}
		else
		{
			double prob3 = u.GetValue() * 10;
			double prob4 = u.GetValue() * 10;
			if(prob3 < prob4)
			{ 
				potential.BETA1 = prob3;
				potential.BETA2 = prob4;
			}
			else
			{
				potential.BETA1 = prob4;
				potential.BETA2 = prob3;
			}
		}
	}
	
	return potential;
}
*/

Ptr<const Packet>
WifiMacQueue::DequeueFirstAvailable (WifiMacHeader *hdr, Time &timestamp,
                                     const QosBlockedDestinations *blockedPackets)
{
  Cleanup ();
  Ptr<const Packet> packet = 0;
  for (PacketQueueI it = m_queue.begin (); it != m_queue.end (); it++)
    {
      if (!it->hdr.IsQosData () ||
          !blockedPackets->IsBlocked (it->hdr.GetAddr1 (), it->hdr.GetQosTid ()))
        {
          *hdr = it->hdr;
          timestamp = it->tstamp;
          packet = it->packet;
          m_queue.erase (it);
          m_size--;
          return packet;
        }
    }
  return packet;
}

Ptr<const Packet>
WifiMacQueue::PeekFirstAvailable (WifiMacHeader *hdr, Time &timestamp,
                                  const QosBlockedDestinations *blockedPackets)
{
  Cleanup ();
  for (PacketQueueI it = m_queue.begin (); it != m_queue.end (); it++)
    {
      if (!it->hdr.IsQosData () ||
          !blockedPackets->IsBlocked (it->hdr.GetAddr1 (), it->hdr.GetQosTid ()))
        {
          *hdr = it->hdr;
          timestamp = it->tstamp;
          return it->packet;
        }
    }
  return 0;
}

} // namespace ns3
