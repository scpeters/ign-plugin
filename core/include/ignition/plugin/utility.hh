/*
 * Copyright (C) 2018 Open Source Robotics Foundation
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


#ifndef IGNITION_PLUGIN_UTILITY_HH_
#define IGNITION_PLUGIN_UTILITY_HH_

#include <string>

#include <ignition/plugin/detail/utility.hh>
#include <ignition/plugin/Export.hh>

namespace ignition
{
  namespace plugin
  {
    /////////////////////////////////////////////////
    /// \brief Contains a static constexpr field named `value` which will be
    /// true if the type `From` has a const-quality less than or equal to the
    /// type `To`.
    ///
    /// The following expressions will return true:
    ///
    /// \code
    ///     ConstCompatible<T, T>::value
    ///     ConstCompatible<const T, T>::value
    /// \endcode
    ///
    /// The following expression will return false:
    ///
    /// \code
    ///     ConstCompatible<T, const T>::value
    /// \endcode
    ///
    template <typename To, typename From>
    using ConstCompatible = detail::ConstCompatible<To, From>;


    /////////////////////////////////////////////////
    /// \brief Demangle the ABI typeinfo name of a symbol into a human-readable
    /// version.
    /// \param[in] _symbol
    ///   Pass in the result of Symbol<T>()
    /// \return The demangled (human-readable) version of the symbol name
    std::string IGNITION_PLUGIN_VISIBLE DemangleSymbol(
        const std::string &_symbol);

    /////////////////////////////////////////////////
    /// \brief Get the raw decorated symbol of the class or struct T.
    /// \return The raw decorated symbol of the class or struct T.
	template <typename T>
	const char * Symbol()
    {
    #ifdef _MSC_VER
      return typeid(T).raw_name();
    #else
      return typeid(T).name();
    #endif
    }
  }
}

#endif
