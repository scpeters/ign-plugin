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


#ifndef IGNITION_PLUGIN_DETAIL_REGISTER_HH_
#define IGNITION_PLUGIN_DETAIL_REGISTER_HH_

#include <memory>
#include <set>
#include <string>
#include <typeinfo>
#include <type_traits>
#include <utility>

#include <ignition/utilities/SuppressWarning.hh>

#include <ignition/plugin/EnablePluginFromThis.hh>
#include <ignition/plugin/Info.hh>
#include <ignition/plugin/utility.hh>
#include <ignition/plugin/detail/IgnitionPluginHook.hh>

namespace ignition
{
  namespace plugin
  {
    namespace detail
    {
      //////////////////////////////////////////////////
      /// \brief This default will be called when NoMoreInterfaces is an empty
      /// parameter pack. When one or more Interfaces are provided, the other
      /// template specialization of this class will be called.
      template <typename PluginClass, typename... NoMoreInterfaces>
      struct InterfaceHelper
      {
        public: static void InsertInterfaces(Info::InterfaceCastingMap &)
        {
          // Do nothing. This is the terminal specialization of the variadic
          // template class member function.
        }
      };

      //////////////////////////////////////////////////
      /// \brief This specialization will be called when one or more Interfaces
      /// are specified.
      template <typename PluginClass, typename Interface,
                typename... RemainingInterfaces>
      struct InterfaceHelper<PluginClass, Interface, RemainingInterfaces...>
      {
        public: static void InsertInterfaces(
          Info::InterfaceCastingMap &interfaces)
        {
          // READ ME: If you get a compilation error here, then one of the
          // interfaces that you tried to register for your plugin is not
          // actually a base class of the plugin class. This is not allowed. A
          // plugin class must inherit every interface class that you want it to
          // provide.
          static_assert(std::is_base_of<Interface, PluginClass>::value,
                        "YOU ARE ATTEMPTING TO REGISTER AN INTERFACE FOR A "
                        "PLUGIN, BUT THE INTERFACE IS NOT A BASE CLASS OF THE "
                        "PLUGIN.");

          interfaces.insert(std::make_pair(
                Symbol<Interface>(),
                [=](void* v_ptr)
                {
                    PluginClass *d_ptr = static_cast<PluginClass*>(v_ptr);
                    return static_cast<Interface*>(d_ptr);
                }));

          InterfaceHelper<PluginClass, RemainingInterfaces...>
              ::InsertInterfaces(interfaces);
        }
      };

      //////////////////////////////////////////////////
      /// \brief This overload will be called when no more aliases remain to be
      /// inserted. If one or more aliases still need to be inserted, then the
      /// overload below this one will be called instead.
      inline void InsertAlias(std::set<std::string> &/*aliases*/)
      {
        // Do nothing. This is the terminal overload of the variadic template
        // function.
      }

      template <typename... Aliases>
      void InsertAlias(std::set<std::string> &aliases,
                       const std::string &nextAlias,
                       Aliases&&... remainingAliases)
      {
        aliases.insert(nextAlias);
        InsertAlias(aliases, std::forward<Aliases>(remainingAliases)...);
      }

      //////////////////////////////////////////////////
      template <typename PluginClass, bool DoEnablePluginFromThis>
      struct IfEnablePluginFromThisImpl
      {
        public: static void AddIt(Info::InterfaceCastingMap &_interfaces)
        {
          _interfaces.insert(std::make_pair(
                  Symbol<EnablePluginFromThis>(),
                  [=](void *v_ptr)
                  {
                    PluginClass *d_ptr = static_cast<PluginClass*>(v_ptr);
                    return static_cast<EnablePluginFromThis*>(d_ptr);
                  }));
        }
      };

      //////////////////////////////////////////////////
      template <typename PluginClass>
      struct IfEnablePluginFromThisImpl<PluginClass, false>
      {
        public: static void AddIt(Info::InterfaceCastingMap &)
        {
          // Do nothing, because the plugin does not inherit
          // the EnablePluginFromThis interface.
        }
      };

      //////////////////////////////////////////////////
      template <typename PluginClass>
      struct IfEnablePluginFromThis
          : IfEnablePluginFromThisImpl<PluginClass,
                std::is_base_of<EnablePluginFromThis, PluginClass>::value>
      { }; // NOLINT

      //////////////////////////////////////////////////
      /// \brief This specialization of the Register class will be called when
      /// one or more arguments are provided to the IGNITION_ADD_PLUGIN(~)
      /// macro. This is the only version of the Registrar class that is allowed
      /// to compile.
      template <typename PluginClass, typename... Interfaces>
      struct Registrar
      {
        public: static Info MakeInfo()
        {
          Info info;

          // Set the name of the plugin
          info.symbol = Symbol<PluginClass>();

          // Create a factory for generating new plugin instances
          info.factory = [=]()
          {
            // vvvvvvvvvvvvvvvvvvvvvvvv  READ ME  vvvvvvvvvvvvvvvvvvvvvvvvvvvvv
            // If you get a compilation error here, then you are trying to
            // register an abstract class as a plugin, which is not allowed. To
            // register a plugin class, every one if its virtual functions must
            // have a definition.
            //
            // Read through the error produced by your compiler to see which
            // pure virtual functions you are neglecting to provide overrides
            // for.
            // ^^^^^^^^^^^^^ READ ABOVE FOR COMPILATION ERRORS ^^^^^^^^^^^^^^^^
            return static_cast<void*>(new PluginClass);
          };

IGN_UTILS_WARN_IGNORE__NON_VIRTUAL_DESTRUCTOR
          // Create a deleter to clean up destroyed instances
          info.deleter = [=](void *ptr)
          {
            delete static_cast<PluginClass*>(ptr);
          };
IGN_UTILS_WARN_RESUME__NON_VIRTUAL_DESTRUCTOR

          // Construct a map from the plugin to its interfaces
          InterfaceHelper<PluginClass, Interfaces...>
              ::InsertInterfaces(info.interfaces);

          return info;
        }

        /// \brief This function registers a plugin along with a set of
        /// interfaces that it provides.
        public: static std::shared_ptr<const void> Register()
        {
          // Make all info that the user has specified
          Info info = MakeInfo();

          // Add the EnablePluginFromThis interface automatically if it is
          // inherited by PluginClass.
          IfEnablePluginFromThis<PluginClass>::AddIt(info.interfaces);

          // Send this information as input to this library's global repository
          // of plugins.
          return IgnitionPluginHook_v1(info, sizeof(Info), alignof(Info));
        }


        public: template <typename... Aliases>
        static void RegisterAlias(Aliases&&... aliases)
        {
          // Dev note (MXG): We expect the RegisterAlias function to be called
          // using the IGNITION_ADD_PLUGIN_ALIAS(~) macro, which should never
          // contain any interfaces. Therefore, this parameter pack should be
          // empty.
          //
          // In the future, we could allow Interfaces and Aliases to be
          // specified simultaneously, but that would be very tricky to do with
          // macros, so for now we will enforce this assumption to make sure
          // that the implementation is working as expected.
          static_assert(sizeof...(Interfaces) == 0,
                        "THERE IS A BUG IN THE ALIAS REGISTRATION "
                        "IMPLEMENTATION! PLEASE REPORT THIS!");

          Info info = MakeInfo();

          // Gather up all the aliases that have been specified for this plugin.
          InsertAlias(info.aliases, std::forward<Aliases>(aliases)...);

          // Send this information as input to this library's global repository
          // of plugins.
          IgnitionPluginHook_v1(info, sizeof(Info), alignof(Info));
        }
      };
    }
  }
}

//////////////////////////////////////////////////
/// This macro creates a uniquely-named class whose constructor calls the
/// ignition::plugin::detail::Registrar::Register function. It then declares a
/// uniquely-named instance of the class with static lifetime. Since the class
/// instance has a static lifetime, it will be constructed when the shared
/// library is loaded. When it is constructed, the Register function will
/// be called.
#define DETAIL_IGNITION_ADD_PLUGIN_HELPER(UniqueID, ...) \
  namespace ignition \
  { \
    namespace plugin \
    { \
      namespace \
      { \
        struct ExecuteWhenLoadingLibrary##UniqueID \
        { \
          ExecuteWhenLoadingLibrary##UniqueID() \
          { \
            handle = \
              ::ignition::plugin::detail::Registrar<__VA_ARGS__>::Register(); \
          } \
          \
          ~ExecuteWhenLoadingLibrary##UniqueID() \
          { \
            ::ignition::plugin::detail::IgnitionPluginHookCleanup_v1(handle); \
          } \
          \
          std::shared_ptr<const void> handle; \
        }; \
  \
        static ExecuteWhenLoadingLibrary##UniqueID execute##UniqueID; \
      } /* namespace */ \
    } \
  }


//////////////////////////////////////////////////
/// This macro is needed to force the __COUNTER__ macro to expand to a value
/// before being passed to the *_HELPER macro.
#define DETAIL_IGNITION_ADD_PLUGIN_WITH_COUNTER(UniqueID, ...) \
  DETAIL_IGNITION_ADD_PLUGIN_HELPER(UniqueID, __VA_ARGS__)


//////////////////////////////////////////////////
/// We use the __COUNTER__ here to give each plugin registration its own unique
/// name, which is required in order to statically initialize each one.
#define DETAIL_IGNITION_ADD_PLUGIN(...) \
  DETAIL_IGNITION_ADD_PLUGIN_WITH_COUNTER(__COUNTER__, __VA_ARGS__)


//////////////////////////////////////////////////
/// This macro creates a uniquely-named class whose constructor calls the
/// ignition::plugin::detail::Registrar::RegisterAlias function. It then
/// declares a uniquely-named instance of the class with static lifetime. Since
/// the class instance has a static lifetime, it will be constructed when the
/// shared library is loaded. When it is constructed, the Register function will
/// be called.
#define DETAIL_IGNITION_ADD_PLUGIN_ALIAS_HELPER(UniqueID, PluginClass, ...) \
  namespace ignition \
  { \
    namespace plugin \
    { \
      namespace \
      { \
        struct ExecuteWhenLoadingLibrary##UniqueID \
        { \
          ExecuteWhenLoadingLibrary##UniqueID() \
          { \
            ::ignition::plugin::detail::Registrar<PluginClass>::RegisterAlias( \
                __VA_ARGS__); \
          } \
        }; \
  \
        static ExecuteWhenLoadingLibrary##UniqueID execute##UniqueID; \
      } /* namespace */ \
    } \
  }


//////////////////////////////////////////////////
/// This macro is needed to force the __COUNTER__ macro to expand to a value
/// before being passed to the *_HELPER macro.
#define DETAIL_IGNITION_ADD_PLUGIN_ALIAS_WITH_COUNTER( \
  UniqueID, PluginClass, ...) \
  DETAIL_IGNITION_ADD_PLUGIN_ALIAS_HELPER(UniqueID, PluginClass, __VA_ARGS__)


//////////////////////////////////////////////////
/// We use the __COUNTER__ here to give each plugin registration its own unique
/// name, which is required in order to statically initialize each one.
#define DETAIL_IGNITION_ADD_PLUGIN_ALIAS(PluginClass, ...) \
  DETAIL_IGNITION_ADD_PLUGIN_ALIAS_WITH_COUNTER( \
  __COUNTER__, PluginClass, __VA_ARGS__)


//////////////////////////////////////////////////
#define DETAIL_IGNITION_ADD_FACTORY(ProductType, FactoryType) \
  DETAIL_IGNITION_ADD_PLUGIN(FactoryType::Producing<ProductType>, FactoryType) \
  DETAIL_IGNITION_ADD_PLUGIN_ALIAS( \
      FactoryType::Producing<ProductType>, \
      ::ignition::plugin::DemangleSymbol( \
        ::ignition::plugin::Symbol<ProductType>()))


//////////////////////////////////////////////////
#define DETAIL_IGNITION_ADD_FACTORY_ALIAS(ProductType, FactoryType, ...) \
  DETAIL_IGNITION_ADD_FACTORY(ProductType, FactoryType) \
  DETAIL_IGNITION_ADD_PLUGIN_ALIAS(FactoryType::Producing<ProductType>, \
      __VA_ARGS__)

#endif
