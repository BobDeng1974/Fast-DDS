// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*!
 * @file AccessPermissionsHandle.h
 */
#ifndef __SECURITY_ACCESSCONTROL_ACCESSPERMISSIONSHANDLE_H__
#define __SECURITY_ACCESSCONTROL_ACCESSPERMISSIONSHANDLE_H__

#include <fastrtps/rtps/security/common/Handle.h>
#include <fastrtps/rtps/common/Token.h>
#include "PermissionsParser.h"
#include <fastrtps/rtps/security/accesscontrol/ParticipantSecurityAttributes.h>

#include <openssl/x509.h>
#include <string>

namespace eprosima {
namespace fastrtps {
namespace rtps {
namespace security {

class AccessPermissions
{
    public:

        AccessPermissions() : store_(nullptr), there_are_crls_(false)  {}

        static const char* const class_id_;

        X509_STORE* store_;
        std::string sn;
        std::string algo;
        bool there_are_crls_;
        PermissionsToken permissions_token_;
        PermissionsCredentialToken permissions_credential_token_;
        ParticipantSecurityAttributes governance;
        Grant grant;
};

typedef HandleImpl<AccessPermissions> AccessPermissionsHandle;

} //namespace security
} //namespace rtps
} //namespace fastrtps
} //namespace eprosima

#endif // __SECURITY_ACCESSCONTROL_ACCESSPERMISSIONSHANDLE_H__
