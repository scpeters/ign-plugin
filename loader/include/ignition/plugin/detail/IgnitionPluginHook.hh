/*
 * Copyright (C) 2019 Open Source Robotics Foundation
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

#ifndef IGNITION_PLUGIN_DETAIL_IGNITIONPLUGINHOOK_HH_
#define IGNITION_PLUGIN_DETAIL_IGNITIONPLUGINHOOK_HH_

#include <cstddef>

#include <ignition/plugin/loader/Export.hh>
#include <ignition/plugin/Info.hh>

namespace ignition
{
namespace plugin
{
namespace v1
{
namespace detail
{
  /// \private IgnitionPluginHook_v2 is the hook that's used by plugin libraries
  /// to send their plugin info to the loader.
  ///
  /// \param[in] _inputInfo
  ///   Plugin information to pass to the loader
  IGNITION_PLUGIN_LOADER_VISIBLE void IgnitionPluginHook(
      const Info &_inputInfo);
}
}
}
}

#endif
