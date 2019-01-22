/*
 * Copyright (C) 2017 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <ignition/plugin/Info.hh>
#include <ignition/plugin/deprecated/v1_Info.hh>
#include <ignition/plugin/utility.hh>

namespace ignition
{
  namespace plugin
  {
    void Info::Clear()
    {
      symbol.clear();
      name.clear();
      aliases.clear();
      interfaces.clear();
      demangledInterfaces.clear();
      factory = nullptr;
      deleter = nullptr;
    }

    namespace v1
    {
      void Info::Clear()
      {
        name.clear();
        aliases.clear();
        interfaces.clear();
        demangledInterfaces.clear();
        factory = nullptr;
        deleter = nullptr;
      }

      info_v1::Info Update(const Info &_oldInfo)
      {
        info_v1::Info newInfo;
        newInfo.symbol = _oldInfo.name;
        newInfo.name = DemangleSymbol(_oldInfo.name);
        newInfo.aliases = _oldInfo.aliases;

        for(const auto &interface : _oldInfo.interfaces)
        {
          newInfo.interfaces.insert(interface);
          newInfo.demangledInterfaces.insert(DemangleSymbol(interface.first));
        }

        newInfo.factory = _oldInfo.factory;
        newInfo.deleter = _oldInfo.deleter;

        return newInfo;
      }
    }
  }
}
