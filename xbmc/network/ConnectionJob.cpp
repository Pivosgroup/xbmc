/*
 *      Copyright (C) 2005-2011 Team XBMC
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

#include "ConnectionJob.h"
#include "utils/log.h"
#include "dialogs/GUIDialogKeyboard.h"
#include "security/KeyringManager.h"

CConnectionJob::CConnectionJob(CConnectionPtr connection, CKeyringManager *keyringManager)
{
  m_connection = connection;
  m_keyringManager = keyringManager;
}

bool CConnectionJob::DoWork()
{
  CIPConfig ipconfig;
  return m_connection->Connect((IPassphraseStorage *)this, ipconfig);
}

void CConnectionJob::InvalidatePassphrase(const std::string &uuid)
{
  m_keyringManager->EraseSecret("network", uuid);
}

bool CConnectionJob::GetPassphrase(const std::string &uuid, std::string &passphrase)
{
  CVariant secret;
  if (m_keyringManager->FindSecret("network", uuid, secret) && secret.isString())
  {
    passphrase = secret.asString();
    return true;
  }
  else
  {
    CStdString utf8;
    bool result = CGUIDialogKeyboard::ShowAndGetNewPassword(utf8);
    passphrase = utf8;
    return result;
  }
}

void CConnectionJob::StorePassphrase(const std::string &uuid, const std::string &passphrase)
{
  m_keyringManager->StoreSecret("network", uuid, CVariant(passphrase));
}
