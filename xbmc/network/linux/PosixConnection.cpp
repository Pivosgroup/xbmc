/*
 *      Copyright (C) 2005-2010 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "PosixConnection.h"
#include "Util.h"
#include "utils/StdString.h"
#include "utils/log.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/wireless.h>
#include <linux/sockios.h>
#include <errno.h>
#include <resolv.h>
#include <net/if_arp.h>
#include <string.h>
#include <vector>

int PosixParseHex(char *str, unsigned char *addr)
{
  int len = 0;

  while (*str)
  {
    int tmp;
    if (str[1] == 0)
      return -1;
    if (sscanf(str, "%02x", (unsigned int *)&tmp) != 1)
      return -1;
    addr[len] = tmp;
    len++;
    str += 2;
  }

  return len;
}

bool PosixGuessIsHex(const char *test_hex, size_t length)
{
  // we could get fooled by strings that only
  // have 0-9, A, B, C, D, E, F in them :)
  for (size_t i = 0; i < length; i++)
  {
    switch (*test_hex++)
    {
      default:
        return false;
        break;
      case '0':case '1':case '2':case '3':case '4':
      case '5':case '6':case '7':case '8':case '9':
      case 'a':case 'A':case 'b':case 'B':
      case 'c':case 'C':case 'd':case 'D':
      case 'e':case 'E':case 'f':case 'F':
        break;
    }
  }

  return true;
}

std::string PosixGetDefaultGateway(const std::string interface)
{
  std::string result = "";

  FILE* fp = fopen("/proc/net/route", "r");
  if (!fp)
    return result;

  char* line     = NULL;
  size_t linel   = 0;
  int n, linenum = 0;
  char   dst[128], iface[16], gateway[128];
  while (getdelim(&line, &linel, '\n', fp) > 0)
  {
    // skip first two lines
    if (linenum++ < 1)
      continue;

    // search where the word begins
    n = sscanf(line, "%16s %128s %128s", iface, dst, gateway);

    if (n < 3)
      continue;

    if (strcmp(iface,   interface.c_str()) == 0 &&
        strcmp(dst,     "00000000") == 0 &&
        strcmp(gateway, "00000000") != 0)
    {
      unsigned char gatewayAddr[4];
      int len = PosixParseHex(gateway, gatewayAddr);
      if (len == 4)
      {
        struct in_addr in;
        in.s_addr = (gatewayAddr[0] << 24) |
          (gatewayAddr[1] << 16) |
          (gatewayAddr[2] << 8)  |
          (gatewayAddr[3]);
        result = inet_ntoa(in);
        break;
      }
    }
  }
  free(line);
  fclose(fp);

  return result;
}

CPosixConnection::CPosixConnection(int socket, const char *interfaceName)
{
  m_socket = socket;
  m_connectionName = interfaceName;

  if (m_connectionName.find("wired") != std::string::npos)
  {
    m_essid = "Wired";
    m_interface = "eth0";
    m_type = NETWORK_CONNECTION_TYPE_WIRED;
  }
  else if (m_connectionName.find("wifi") != std::string::npos)
  {
    std::string::size_type start = m_connectionName.find("_") + 1;
    std::string::size_type end   = m_connectionName.find("_", start + 1);
    m_essid = m_connectionName.substr(start, end - start);
    m_interface = "wlan0";
    m_type = NETWORK_CONNECTION_TYPE_WIFI;
    if (m_connectionName.find("_wpa2") != std::string::npos)
      m_encryption = NETWORK_CONNECTION_ENCRYPTION_WPA2;
    else if (m_connectionName.find("_wpa") != std::string::npos)
      m_encryption = NETWORK_CONNECTION_ENCRYPTION_WPA;
    else if (m_connectionName.find("_wep") != std::string::npos)
      m_encryption = NETWORK_CONNECTION_ENCRYPTION_WEP;
    else
      m_encryption = NETWORK_CONNECTION_ENCRYPTION_NONE;
  }
  else
  {
    m_essid = "Unknown";
    m_interface = "unknown";
    m_type = NETWORK_CONNECTION_TYPE_UNKNOWN;
    m_encryption = NETWORK_CONNECTION_ENCRYPTION_UNKNOWN;
  }

  m_state = GetConnectionState();
}

CPosixConnection::~CPosixConnection()
{
}

bool CPosixConnection::Connect(IPassphraseStorage *storage, CIPConfig &ipconfig)
{
  //printf("CPosixConnection::Connect %s\n", m_connectionName.c_str());
  ipconfig.m_method     = IP_CONFIG_DHCP;
  ipconfig.m_address    = m_address;
  ipconfig.m_netmask    = m_netmask;
  ipconfig.m_gateway    = m_gateway;
  ipconfig.m_interface  = m_interface;
  ipconfig.m_essid      = m_essid;
  ipconfig.m_encryption = m_encryption;

  if (m_type == NETWORK_CONNECTION_TYPE_WIRED)
  {
    SetSettings(ipconfig);
    return true;
  }
  else if (m_type == NETWORK_CONNECTION_TYPE_WIFI)
  {
    std::string command, result, passphrase;
    if (m_encryption != NETWORK_CONNECTION_ENCRYPTION_NONE)
    {
      if (!storage->GetPassphrase(m_connectionName, passphrase))
        return false;
    }
    ipconfig.m_passphrase = passphrase;
    SetSettings(ipconfig);

    return true;
  }

  return false;
}

ConnectionState CPosixConnection::GetConnectionState() const
{
  int zero = 0;
  struct ifreq ifr;

  // check if the interface is up.
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, m_interface.c_str());
  if (ioctl(m_socket, SIOCGIFFLAGS, &ifr) < 0)
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  // check for running and not loopback
  if (!(ifr.ifr_flags & IFF_RUNNING) || (ifr.ifr_flags & IFF_LOOPBACK))
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  // check for an ip address
  if (ioctl(m_socket, SIOCGIFADDR, &ifr) < 0)
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  if (ifr.ifr_addr.sa_data == NULL)
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  // return only interfaces which have an ip address
  if (memcmp(ifr.ifr_addr.sa_data + sizeof(short), &zero, sizeof(int)) == 0)
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  if (m_type == NETWORK_CONNECTION_TYPE_WIFI)
  {
    // for wifi, we need to check we have a wifi driver name.
    struct iwreq wrq;
    strcpy(wrq.ifr_name, m_interface.c_str());
    if (ioctl(m_socket, SIOCGIWNAME, &wrq) < 0)
      return NETWORK_CONNECTION_STATE_DISCONNECTED;

    // since the wifi interface (wlan0) can be connected to
    // any wifi access point, we need to compare the assigned
    // essid to our connection essid. If they match, then
    // this connection is up.
    char essid[IFNAMSIZ];
    memset(&wrq, 0, sizeof(struct iwreq));
    wrq.u.essid.pointer = (caddr_t)essid;
    wrq.u.essid.length  = sizeof(essid);
    strncpy(wrq.ifr_name, m_interface.c_str(), IFNAMSIZ);
    if (ioctl(m_socket, SIOCGIWESSID, &wrq) < 0)
      return NETWORK_CONNECTION_STATE_DISCONNECTED;

    if (wrq.u.essid.length <= 0)
      return NETWORK_CONNECTION_STATE_DISCONNECTED;

    std::string test_essid(essid, wrq.u.essid.length);
    if (m_essid.find(test_essid) == std::string::npos)
      return NETWORK_CONNECTION_STATE_DISCONNECTED;
  }

  // finally, we need to see if we have a gateway assigned to our interface.
  std::string default_gateway = PosixGetDefaultGateway(m_interface);
  if (default_gateway.size() <= 0)
    return NETWORK_CONNECTION_STATE_DISCONNECTED;

  //printf("CPosixConnection::GetConnectionState, %s: we are up\n", m_connectionName.c_str());

  // passing the above tests means we are connected.
  return NETWORK_CONNECTION_STATE_CONNECTED;
}

std::string CPosixConnection::GetName() const
{
  return m_essid;
}

std::string CPosixConnection::GetAddress() const
{
  struct ifreq ifr;
  strcpy(ifr.ifr_name, m_interface.c_str());
  ifr.ifr_addr.sa_family = AF_INET;

  if (ioctl(m_socket, SIOCGIFADDR, &ifr) >= 0)
    return inet_ntoa((*((struct sockaddr_in*)&ifr.ifr_addr)).sin_addr);
  else
    return "";
}

std::string CPosixConnection::GetNetmask() const
{
  struct ifreq ifr;
  strcpy(ifr.ifr_name, m_interface.c_str());
  ifr.ifr_addr.sa_family = AF_INET;

  if (ioctl(m_socket, SIOCGIFNETMASK, &ifr) >= 0)
    return inet_ntoa((*((struct sockaddr_in*)&ifr.ifr_addr)).sin_addr);
  else
    return "";
}

std::string CPosixConnection::GetGateway() const
{
  return PosixGetDefaultGateway(m_interface);
}

std::string CPosixConnection::GetMacAddress() const
{
  CStdString result;
  result.Format("00:00:00:00:00:00");

  struct ifreq ifr;
  strcpy(ifr.ifr_name, m_interface.c_str());
  if (ioctl(m_socket, SIOCGIFHWADDR, &ifr) >= 0)
  {
    result.Format("%02X:%02X:%02X:%02X:%02X:%02X",
      ifr.ifr_hwaddr.sa_data[0],
      ifr.ifr_hwaddr.sa_data[1],
      ifr.ifr_hwaddr.sa_data[2],
      ifr.ifr_hwaddr.sa_data[3],
      ifr.ifr_hwaddr.sa_data[4],
      ifr.ifr_hwaddr.sa_data[5]);
  }

  return result.c_str();
}

unsigned int CPosixConnection::GetStrength() const
{
  int strength = 100;
  if (m_type == NETWORK_CONNECTION_TYPE_WIFI)
  {
    struct iwreq wreq;
    // wireless tools says this is large enough
    char   buffer[sizeof(struct iw_range) * 2];
    int max_qual_level = 0;
    double max_qual = 92.0;

    // Fetch the range
    memset(buffer, 0, sizeof(iw_range) * 2);
    memset(&wreq,  0, sizeof(struct iwreq));
    wreq.u.data.pointer = (caddr_t)buffer;
    wreq.u.data.length  = sizeof(buffer);
    wreq.u.data.flags   = 0;
    strncpy(wreq.ifr_name, m_interface.c_str(), IFNAMSIZ);
    if (ioctl(m_socket, SIOCGIWRANGE, &wreq) >= 0)
    {
      struct iw_range *range = (struct iw_range*)buffer;
      if (range->max_qual.qual > 0)
        max_qual = range->max_qual.qual;
      if (range->max_qual.level > 0)
        max_qual_level = range->max_qual.level;
    }

    struct iw_statistics stats;
    memset(&wreq, 0, sizeof(struct iwreq));
    // Fetch the stats
    wreq.u.data.pointer = (caddr_t)&stats;
    wreq.u.data.length  = sizeof(stats);
    wreq.u.data.flags   = 1;     // Clear updated flag
    strncpy(wreq.ifr_name, m_interface.c_str(), IFNAMSIZ);
    if (ioctl(m_socket, SIOCGIWSTATS, &wreq) < 0) {
        printf("Failed to fetch signal stats, %s", strerror(errno));
        return 0;
    }

    // this is not correct :)
    strength = (100 * wreq.u.qual.qual)/256;

    //printf("CPosixConnection::GetStrength, strength(%d)\n", strength);
  }
  return strength;
}

EncryptionType CPosixConnection::GetEncryption() const
{
  return m_encryption;
}

unsigned int CPosixConnection::GetConnectionSpeed() const
{
  int speed = 100;
  return speed;
}

ConnectionType CPosixConnection::GetConnectionType() const
{
  return m_type;
}

bool CPosixConnection::PumpNetworkEvents()
{
  bool state_changed = false;

  ConnectionState state = GetConnectionState();
  if (m_state != state)
  {
    //printf("CPosixConnection::PumpNetworkEvents, m_connectionName(%s), m_state(%d) -> state(%d)\n",
    //  m_connectionName.c_str(), m_state, state);
    m_state = state;
    state_changed = true;
  }

  return state_changed;
}

void CPosixConnection::GetSettings(CIPConfig &ipconfig)
{
/*
  ipconfig.reset();

  FILE* fp = fopen("/etc/network/interfaces", "r");
  if (!fp)
  {
    // TODO
    return;
  }

  CStdString s;
  size_t linel = 0;
  char*  line  = NULL;
  bool   foundInterface = false;

  while (getdelim(&line, &linel, '\n', fp) > 0)
  {
    std::vector<CStdString> tokens;

    s = line;
    s.TrimLeft(" \t").TrimRight(" \n");

    // skip comments
    if (s.length() == 0 || s.GetAt(0) == '#')
      continue;

    // look for "iface <interface name> inet"
    CUtil::Tokenize(s, tokens, " ");
    if (!foundInterface && tokens.size() >=3 && tokens[0].Equals("iface") &&
      tokens[1].Equals(ipconfig.m_interface.c_str()) && tokens[2].Equals("inet"))
    {
      if (tokens[3].Equals("dhcp"))
      {
        ipconfig.m_method = IP_CONFIG_DHCP;
        foundInterface = true;
      }
      if (tokens[3].Equals("static"))
      {
        ipconfig.m_method = IP_CONFIG_STATIC;
        foundInterface = true;
      }
    }

    if (foundInterface && tokens.size() == 2)
    {
      if (tokens[0].Equals("address"))
        ipconfig.m_address = tokens[1];
      else if (tokens[0].Equals("netmask"))
        ipconfig.m_netmask = tokens[1];
      else if (tokens[0].Equals("gateway"))
        ipconfig.m_gateway = tokens[1];
      else if (tokens[0].Equals("wireless-essid"))
        ipconfig.m_essid   = tokens[1];
      else if (tokens[0].Equals("wireless-key"))
      {
        CStdString key;
        key = tokens[1];
        if (key.length() > 2 && key[0] == 's' && key[1] == ':')
          key.erase(0, 2);
        ipconfig.m_passphrase = key;
        ipconfig.m_encryption = NETWORK_CONNECTION_ENCRYPTION_WEP;
      }
      else if (tokens[0].Equals("wpa-ssid"))
        ipconfig.m_essid = tokens[1];
      else if (tokens[0].Equals("wpa-proto") && tokens[1].Equals("WPA"))
        ipconfig.m_encryption = NETWORK_CONNECTION_ENCRYPTION_WPA;
      else if (tokens[0].Equals("wpa-proto") && tokens[1].Equals("WPA2"))
        ipconfig.m_encryption = NETWORK_CONNECTION_ENCRYPTION_WPA2;
      else if (tokens[0].Equals("wpa-psk"))
        ipconfig.m_passphrase = tokens[1];
      else if (tokens[0].Equals("auto") || tokens[0].Equals("iface") || tokens[0].Equals("mapping"))
        break;
    }
  }
  free(line);

  // Fallback in case wpa-proto is not set
  if (ipconfig.m_passphrase != "" && ipconfig.m_encryption == NETWORK_CONNECTION_ENCRYPTION_NONE)
    ipconfig.m_encryption = NETWORK_CONNECTION_ENCRYPTION_WPA;

  fclose(fp);
*/
}

void CPosixConnection::SetSettings(const CIPConfig &ipconfig)
{
  //printf("CPosixConnection::SetSettings %s, method(%d)\n",
  //  m_connectionName.c_str(), ipconfig.m_method);
  FILE *fr = fopen("/etc/network/interfaces", "r");
  if (!fr)
  {
    // TODO
    return;
  }

  char *line = NULL;
  size_t line_length = 0;
  std::vector<std::string> interfaces_lines;
  while (getdelim(&line, &line_length, '\n', fr) > 0)
    interfaces_lines.push_back(line);
  fclose(fr);

  std::vector<std::string> new_interfaces_lines;
  std::vector<std::string> ifdown_interfaces;
  for (size_t i = 0; i < interfaces_lines.size(); i++)
  {
    //printf("CPosixConnection::SetSettings, interfaces_lines:%s", interfaces_lines[i].c_str());
    // always copy auto section over
    if (interfaces_lines[i].find("auto") != std::string::npos)
    {
      new_interfaces_lines.push_back(interfaces_lines[i]);
      continue;
    }

    // always copy loopback iface section over
    if (interfaces_lines[i].find("iface lo") != std::string::npos)
    {
      new_interfaces_lines.push_back(interfaces_lines[i]);
      continue;
    }

    // look for "iface <interface name> inet"
    if (interfaces_lines[i].find("iface") != std::string::npos)
    {
      // we always copy the iface line over.
      new_interfaces_lines.push_back(interfaces_lines[i]);

      // we will take all interfaces down, then bring up this one.
      // so find all iface names.
      std::string ifdown_interface = interfaces_lines[i];
      std::string::size_type start = ifdown_interface.find("iface") + sizeof("iface");
      std::string::size_type end   = ifdown_interface.find("inet", start);
      ifdown_interfaces.push_back(ifdown_interface.substr(start, end - start));

      // is this our interface section (eth0 or wlan0)
      if (interfaces_lines[i].find(ipconfig.m_interface) != std::string::npos)
      {
        // we only touch wlan0 (wifi) settings.
        if (m_type == NETWORK_CONNECTION_TYPE_WIFI)
        {
          std::string tmp;
          if (m_encryption == NETWORK_CONNECTION_ENCRYPTION_NONE)
          {
            tmp = "  wireless-essid \"" + ipconfig.m_essid + "\"\n";
            new_interfaces_lines.push_back(tmp);
          }
          else if (m_encryption == NETWORK_CONNECTION_ENCRYPTION_WEP)
          {
            tmp = "  wireless-essid \"" + ipconfig.m_essid + "\"\n";
            new_interfaces_lines.push_back(tmp);
            tmp = "  wireless-key s:" + ipconfig.m_essid + "\n";
            new_interfaces_lines.push_back(tmp);
          }
          else if (m_encryption == NETWORK_CONNECTION_ENCRYPTION_WPA ||
            m_encryption == NETWORK_CONNECTION_ENCRYPTION_WPA2)
          {
            tmp = "  wpa-ssid \"" + ipconfig.m_essid + "\"\n";
            new_interfaces_lines.push_back(tmp);
            // if ascii, then quote it, if hex, no quotes
            if (PosixGuessIsHex(ipconfig.m_passphrase.c_str(), ipconfig.m_passphrase.size()))
              tmp = "  wpa-psk " + ipconfig.m_passphrase + "\n";
            else
              tmp = "  wpa-psk \"" + ipconfig.m_passphrase + "\"\n";
            new_interfaces_lines.push_back(tmp);
            if (ipconfig.m_encryption == NETWORK_CONNECTION_ENCRYPTION_WPA)
              tmp = "  wpa-proto WPA\n";
            else
              tmp = "  wpa-proto WPA2\n";
            new_interfaces_lines.push_back(tmp);
          }
        }
      }
    }
  }

  FILE* fw = fopen("/etc/network/interfaces.temp", "w");
  if (!fw)
  {
    // TODO
    return;
  }
  for (size_t i = 0; i < new_interfaces_lines.size(); i++)
  {
    printf("CPosixConnection::SetSettings, new_interfaces_lines:%s", new_interfaces_lines[i].c_str());
    fwrite(new_interfaces_lines[i].c_str(), new_interfaces_lines[i].size(), 1, fw);
  }
  fclose(fw);

  // Rename the file (remember, you can not rename across devices)
  if (rename("/etc/network/interfaces.temp", "/etc/network/interfaces") < 0)
  {
    printf("CPosixConnection::SetSettings, rename failed, %s\n", strerror(errno));
    // TODO
    return;
  }

  int rtn_error;
  std::string cmd;
  for (size_t i = 0; i < ifdown_interfaces.size(); i++)
  {
    cmd = "/sbin/ifdown " + ifdown_interfaces[i];
    rtn_error = system(cmd.c_str());
    if (rtn_error != 0 && rtn_error != ECHILD)
      CLog::Log(LOGERROR, "Unable to stop interface %s, %s", ifdown_interfaces[i].c_str(), strerror(errno));
    else
      CLog::Log(LOGINFO, "Stopped interface %s", ifdown_interfaces[i].c_str());
  }

  cmd = "/sbin/ifup " + ipconfig.m_interface;
  rtn_error = system(cmd.c_str());
  if (rtn_error != 0 && rtn_error != ECHILD)
    CLog::Log(LOGERROR, "Unable to start interface %s, %s", ipconfig.m_interface.c_str(), strerror(errno));
  else
    CLog::Log(LOGINFO, "Started interface %s", ipconfig.m_interface.c_str());
}
