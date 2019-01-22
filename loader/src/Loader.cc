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

#include <dlfcn.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <locale>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <ignition/plugin/Info.hh>
#include <ignition/plugin/Loader.hh>
#include <ignition/plugin/Plugin.hh>
#include <ignition/plugin/utility.hh>
#include <ignition/plugin/detail/IgnitionPluginHook.hh>

#include <ignition/plugin/deprecated/v1_Info.hh>

namespace ignition
{
  namespace plugin
  {
    /////////////////////////////////////////////////
    struct Registry
    {
      public: void clear()
      {
        pluginMap.clear();
      }

      public: ignition::plugin::InfoMap pluginMap;
    };

    /// \brief This static variable is used to determine whether we are
    /// currently registering native plugins or external plugins
    /// that get dynamically loaded from shared libraries. This will be set to
    /// true while each Loader is dynamically loading a library.
    static bool registeringDynamicPlugins = false;

    /// \brief This static variable is used by IgnitionPluginHook_v# to indicate
    /// whether the current plugin registration went okay.
    static bool registrationOkay = false;

    /// \brief This collection of plugins is native to the current
    /// application. They were not loaded from a dynamically linked library,
    /// but rather they belong to a statically linked library or translation
    /// unit belonging to the application.
    ///
    /// Every Loader instance will have access to these plugins.
    static Registry kNativePlugins;

    /// \brief This collection of plugins has been loaded from a shared
    /// library. This gets cleared at the end of each call to LoadLib(~) so
    /// that Loader objects only get filled with the plugins that the user has
    /// asked for it to be filled with.
    ///
    /// Only the Loader instances that asked to load the library of these
    /// plugins will have access to these plugins.
    static Registry kDynamicPlugins;

    struct Archive
    {
      using WeakConstInfoPtr = std::weak_ptr<const Info>;

      using DlHandleToInfo =
        std::unordered_map<const void*, std::vector<WeakConstInfoPtr>>;
      DlHandleToInfo dlHandleToInfo;

      using InfoToDlHandle = std::unordered_map<const void*, void*>;
      InfoToDlHandle infoToDlHandle;
    };

    /// \brief This static variable holds weak references to plugin information
    /// from all the libraries that have been loaded in the past. If the library
    /// is still loaded, then the weak references
    static Archive kArchive;


    /////////////////////////////////////////////////
    std::vector<ConstInfoPtr> ExtractRegistry(const Registry &_registry)
    {
      std::vector<ConstInfoPtr> info;
      for (const auto &entry : _registry.pluginMap)
        info.push_back(entry.second);

      return info;
    }

    /////////////////////////////////////////////////
    void ArchivePluginInfo(
        const std::vector<ConstInfoPtr> &_info,
        const std::shared_ptr<void> &_dlHandle)
    {
      // If there is no plugin info, then the library that was loaded was not a
      // plugin library and should not be given an archive entry at all. The
      // archive relies on plugin libraries to clean themselves out of the
      // archive when they unload, so adding non-plugin libraries to the archive
      // will pollute it and lead to bugs in the future.
      if (_info.empty())
        return;

      auto &archiveEntry = kArchive.dlHandleToInfo[_dlHandle.get()];

      for (const auto &entry : _info)
      {
        archiveEntry.push_back(entry);
        kArchive.infoToDlHandle[entry.get()] = _dlHandle.get();
      }
    }

    /////////////////////////////////////////////////
    /// \brief PIMPL Implementation of the Loader class
    class Loader::Implementation
    {
      /// Constructor. This will be used to store the native plugins that get
      /// loaded during the initialization of the application.
      public: Implementation();

      /// \brief Attempt to load a library at the given path.
      /// \param[in] _pathToLibrary The full path to the desired library
      /// \return If a library exists at the given path, get a point to its dl
      /// handle. If the library does not exist, get a nullptr.
      public: std::shared_ptr<void> CreateDlHandle(
        const std::string &_pathToLibrary);

      /// \brief The pointer to the IgnitionPluginHook function of the loaded
      /// library.
      public: std::vector<ConstInfoPtr> OldIgnitionHook(
          void *_infoFuncPtr,
          const std::string &_pathToLibrary) const;

      /// \brief Using a dl handle produced by LoadLib, extract the
      /// Info from the loaded library.
      /// \param[in] _dlHandle A handle produced by LoadLib
      /// \param[in] _pathToLibrary The path that the library was loaded from
      /// (used for debug purposes)
      /// \return All the Info provided by the loaded library.
      public: std::vector<ConstInfoPtr> ReceivePlugins(
        const std::shared_ptr<void> &_dlHandle,
        const std::string &_pathToLibrary) const;

      public: std::unordered_set<std::string> StorePlugins(
          std::vector<ConstInfoPtr> _loadedPlugins,
          const std::shared_ptr<void> _dlHandle);

      /// \sa Loader::ForgetLibrary()
      public: bool ForgetLibrary(void *_dlHandle);

      /// \brief Pass in a plugin name or alias, and this will give back the
      /// plugin name that corresponds to it. If the name or alias could not be
      /// found, this returns an empty string.
      /// \return The demangled symbol name of the desired plugin, or an empty
      /// string if no matching plugin could be found.
      public: std::string LookupPlugin(const std::string &_nameOrAlias) const;

      public: using AliasMap = std::map<std::string, std::set<std::string>>;
      /// \brief A map from known alias names to the plugin names that they
      /// correspond to. Since an alias might refer to more than one plugin, the
      /// key of this map is a set.
      public: AliasMap aliases;

      public: using PluginToDlHandleMap =
          std::unordered_map< std::string, std::shared_ptr<void> >;
      /// \brief A map from known plugin names to the handle of the library that
      /// provides it.
      ///
      /// CRUCIAL DEV NOTE (MXG): `pluginToDlHandlePtrs` MUST come BEFORE
      /// `plugins` in this class definition to ensure that `plugins` gets
      /// deleted first (member variables get destructed in the reverse order of
      /// their appearance in the class definition). The destructors of the
      /// `deleter` members of the Info class depend on the shared library
      /// still being available, so this map of std::shared_ptrs to the library
      /// handles must be destroyed after the Info.
      ///
      /// If you change this class definition for ANY reason, be sure to
      /// maintain the ordering of these member variables.
      public: PluginToDlHandleMap pluginToDlHandlePtrs;

      public: using PluginMap = std::unordered_map<std::string, ConstInfoPtr>;
      /// \brief A map from known plugin names to their Info
      ///
      /// CRUCIAL DEV NOTE (MXG): `plugins` MUST come AFTER
      /// `pluginToDlHandlePtrs` in this class definition. See the comment on
      /// pluginToDlHandlePtrs for an explanation.
      ///
      /// If you change this class definition for ANY reason, be sure to
      /// maintain the ordering of these member variables.
      public: PluginMap plugins;

      using DlHandleMap = std::unordered_map< void*, std::weak_ptr<void> >;
      /// \brief A map which keeps track of which shared libraries have been
      /// loaded by this Loader.
      ///
      /// The key of this map is a pointer to a dl handle, and the values are
      /// weak_ptrs to the shared_ptr that was constructed for that dl handle.
      /// Since these are weak_ptrs, they will automatically get cleared
      /// whenever the library get unloaded. Therefore, if a map entry exists
      /// for a dl handle and the value of that map entry still points to a
      /// valid shared_ptr, then the library for that dl handle is currently
      /// loaded, and we are already managing its reference count.
      ///
      /// This is used to ensure that we keep one single authoritative reference
      /// count of the dl handle per Loader.
      public: DlHandleMap dlHandlePtrMap;

      public: using DlHandleToPluginMap =
          std::unordered_map< void*, std::unordered_set<std::string> >;
      /// \brief A map from the shared library handle to the names of the
      /// plugins that it provides.
      public: DlHandleToPluginMap dlHandleToPluginMap;

      /// \brief A mutex that gets locked while a library is being loaded. This
      /// ensures that plugins get registered only into the loaders that
      /// specifically requested them.
      public: static std::mutex loadingMutex;
    };

    std::mutex Loader::Implementation::loadingMutex;

    /////////////////////////////////////////////////
    std::string Loader::PrettyStr() const
    {
      auto interfaces = this->InterfacesImplemented();
      std::stringstream pretty;
      pretty << "Loader State" << std::endl;
      pretty << "\tKnown Interfaces: " << interfaces.size() << std::endl;
      for (auto const &interface : interfaces)
        pretty << "\t\t" << interface << std::endl;

      pretty << "\tKnown Plugins: " << dataPtr->plugins.size() << std::endl;
      for (const auto &pair : dataPtr->plugins)
      {
        const ConstInfoPtr &plugin = pair.second;
        const std::size_t aSize = plugin->aliases.size();

        pretty << "\t\t[" << plugin->name << "]\n";
        if (0 < aSize)
        {
          pretty << "\t\t\thas "
                 << aSize << (aSize == 1? " alias" : " aliases") << ":\n";
          for (const auto &alias : plugin->aliases)
            pretty << "\t\t\t\t[" << alias << "]\n";
        }
        else
        {
          pretty << "has no aliases\n";
        }

        const std::size_t iSize = plugin->interfaces.size();
        pretty << "\t\t\timplements " << iSize
               << (iSize == 1? " interface" : " interfaces") << ":\n";
        for (const auto &interface : plugin->demangledInterfaces)
          pretty << "\t\t\t\t" << interface << "\n";
      }

      Implementation::AliasMap badAliases;
      for (const auto &entry : this->dataPtr->aliases)
      {
        if (entry.second.size() > 1)
        {
          badAliases.insert(entry);
        }
      }

      if (!badAliases.empty())
      {
        const std::size_t aSize = badAliases.size();
        pretty << "\tThere " << (aSize == 1? "is " : "are ")  << aSize
               << (aSize == 1? " alias" : " aliases") << " with a "
               << "name collision:\n";
        for (const auto &alias : badAliases)
        {
          pretty << "\t\t[" << alias.first << "] collides between:\n";
          for (const auto &name : alias.second)
            pretty << "\t\t\t[" << name << "]\n";
        }
      }

      pretty << std::endl;

      return pretty.str();
    }

    /////////////////////////////////////////////////
    Loader::Loader()
      : dataPtr(new Implementation())
    {
      // Do nothing.
    }

    /////////////////////////////////////////////////
    Loader::~Loader()
    {
      // Do nothing.
    }

    /////////////////////////////////////////////////
    std::unordered_set<std::string> CheckForNativePluginSymbols(
        const std::shared_ptr<void> &_dlHandle)
    {
      std::unordered_set<std::string> plugins;

      for (const auto &entry : kNativePlugins.pluginMap)
      {
        const auto &info = entry.second;

        std::string typeSymbol;

      #if defined(_MSC_VER)
        // TODO(MXG): Find out how to interpret class symbol names for MSVC
        // libraries
      #else
        // If the compiler is not MSVC, we'll assume that the Itanium ABI is
        // being used. That's the ABI used by GCC and Clang.
        typeSymbol = "_ZTI" + info->symbol;
      #endif

        void *symbolHandle = dlsym(_dlHandle.get(), typeSymbol.c_str());
        if (symbolHandle)
          plugins.insert(info->name);
      }

    #if not defined(__GNUC__) && not defined(__clang__) && not defined(_MSC_VER)
      if (plugins.empty())
      {
        std::cerr << "[ignition::plugin::Loader::LoadLib] Warning: Your "
                  << "compiler was not recognized, so "
                  << "ignition::plugin::Loader::LoadLib might not work as "
                  << "intended!\n";
      }
    #endif

      return plugins;
    }

    /////////////////////////////////////////////////
    std::unordered_set<std::string> Loader::LoadLib(
        const std::string &_pathToLibrary)
    {
      std::unique_lock<std::mutex> lock(Implementation::loadingMutex);

      registeringDynamicPlugins = true;
      registrationOkay = true;
      // Attempt to load the library at this path
      const std::shared_ptr<void> &dlHandle =
          this->dataPtr->CreateDlHandle(_pathToLibrary);
      registeringDynamicPlugins = false;

      if (!registrationOkay)
      {
        std::cerr << "A plugin registration error was encountered while trying "
                  << "to load the library [" << _pathToLibrary << "]\n";
      }

      // Quit early and return an empty set of plugin names if we did not
      // actually get a valid dlHandle.
      if (nullptr == dlHandle)
        return {};

      std::unordered_set<std::string> loadedPlugins =
          this->dataPtr->StorePlugins(
            this->dataPtr->ReceivePlugins(dlHandle, _pathToLibrary), dlHandle);

      // Clear the list of dynamically loaded plugins
      kDynamicPlugins.clear();

      if (loadedPlugins.empty())
      {
        // The list of loaded plugins may be empty if this is a library that was
        // linked to the application at compile-time, and therefore its plugins
        // were already loaded at the time the application initialized. To find
        // out if this is the case, we will search to see if any of the native
        // plugin symbols can be found in this library.
        loadedPlugins = CheckForNativePluginSymbols(dlHandle);

        if (loadedPlugins.empty())
        {
          std::cerr << "The plugin library [" << _pathToLibrary << "] failed "
                    << "to load any plugins!\n";
        }
      }

      return loadedPlugins;
    }

    /////////////////////////////////////////////////
    std::unordered_set<std::string> Loader::InterfacesImplemented() const
    {
      std::unordered_set<std::string> interfaces;
      for (auto const &plugin : this->dataPtr->plugins)
      {
        for (auto const &interface : plugin.second->demangledInterfaces)
          interfaces.insert(interface);
      }
      return interfaces;
    }

    /////////////////////////////////////////////////
    std::unordered_set<std::string> Loader::PluginsImplementing(
        const std::string &_interface,
        const bool demangled) const
    {
      std::unordered_set<std::string> plugins;

      if (demangled)
      {
        for (auto const &plugin : this->dataPtr->plugins)
        {
          if (plugin.second->demangledInterfaces.find(_interface) !=
              plugin.second->demangledInterfaces.end())
            plugins.insert(plugin.second->name);
        }
      }
      else
      {
        for (auto const &plugin : this->dataPtr->plugins)
        {
          if (plugin.second->interfaces.find(_interface) !=
              plugin.second->interfaces.end())
            plugins.insert(plugin.second->name);
        }
      }

      return plugins;
    }

    /////////////////////////////////////////////////
    std::set<std::string> Loader::AllPlugins() const
    {
      std::set<std::string> result;

      for (const auto &entry : this->dataPtr->plugins)
        result.insert(result.end(), entry.first);

      return result;
    }

    /////////////////////////////////////////////////
    std::set<std::string> Loader::PluginsWithAlias(
        const std::string &_alias) const
    {
      std::set<std::string> result;

      const Implementation::AliasMap::const_iterator names =
          this->dataPtr->aliases.find(_alias);

      if (names != this->dataPtr->aliases.end())
        result = names->second;

      const Implementation::PluginMap::const_iterator plugin =
          this->dataPtr->plugins.find(_alias);

      if (plugin != this->dataPtr->plugins.end())
        result.insert(_alias);

      return result;
    }

    /////////////////////////////////////////////////
    std::set<std::string> Loader::AliasesOfPlugin(
        const std::string &_pluginName) const
    {
      const Implementation::PluginMap::const_iterator plugin =
          this->dataPtr->plugins.find(_pluginName);

      if (plugin != this->dataPtr->plugins.end())
        return plugin->second->aliases;

      return {};
    }

    /////////////////////////////////////////////////
    std::string Loader::LookupPlugin(const std::string &_nameOrAlias) const
    {
      return this->dataPtr->LookupPlugin(_nameOrAlias);
    }

    /////////////////////////////////////////////////
    PluginPtr Loader::Instantiate(const std::string &_pluginNameOrAlias) const
    {
      const std::string &resolvedName = this->LookupPlugin(_pluginNameOrAlias);
      if (resolvedName.empty())
        return PluginPtr();

      PluginPtr ptr(this->PrivateGetInfo(resolvedName),
                    this->PrivateGetPluginDlHandlePtr(resolvedName));

      if (auto *enableFromThis = ptr->QueryInterface<EnablePluginFromThis>())
        enableFromThis->PrivateSetPluginFromThis(ptr);

      return ptr;
    }

    /////////////////////////////////////////////////
    bool Loader::ForgetLibrary(const std::string &_pathToLibrary)
    {
#ifndef RTLD_NOLOAD
// This macro is not part of the POSIX standard, and is a custom addition to
// glibc-2.2, so we need create a no-op stand-in flag for it if we are not
// using glibc-2.2.
#define RTLD_NOLOAD 0
#endif

      void *dlHandle = dlopen(_pathToLibrary.c_str(),
                              RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);

      if (!dlHandle)
        return false;

      // We should decrement the reference count because we called dlopen. Even
      // with the RTLD_NOLOAD flag, the call to dlopen will still (allegedly)
      // increment the reference count when it returns a valid handle. Note that
      // this knowledge is according to online discussions and is not explicitly
      // stated in the manual pages of dlopen (but it is consistent with the
      // overall behavior of dlopen).
      dlclose(dlHandle);

      return this->dataPtr->ForgetLibrary(dlHandle);
    }

    /////////////////////////////////////////////////
    bool Loader::ForgetLibraryOfPlugin(const std::string &_pluginNameOrAlias)
    {
      const std::string &resolvedName = this->LookupPlugin(_pluginNameOrAlias);

      Implementation::PluginToDlHandleMap::iterator it =
          dataPtr->pluginToDlHandlePtrs.find(resolvedName);

      if (dataPtr->pluginToDlHandlePtrs.end() == it)
        return false;

      return dataPtr->ForgetLibrary(it->second.get());
    }

    /////////////////////////////////////////////////
    ConstInfoPtr Loader::PrivateGetInfo(
        const std::string &_resolvedName) const
    {
      const Implementation::PluginMap::const_iterator it =
          this->dataPtr->plugins.find(_resolvedName);

      if (this->dataPtr->plugins.end() == it)
      {
        // LCOV_EXCL_START
        std::cerr << "[ignition::Loader::PrivateGetInfo] A resolved name ["
                  << _resolvedName << "] could not be found in the PluginMap. "
                  << "This should not be possible! Please report this bug!\n";
        assert(false);
        return nullptr;
        // LCOV_EXCL_STOP
      }

      return it->second;
    }

    /////////////////////////////////////////////////
    std::shared_ptr<void> Loader::PrivateGetPluginDlHandlePtr(
        const std::string &_resolvedName) const
    {
      Implementation::PluginToDlHandleMap::iterator it =
          dataPtr->pluginToDlHandlePtrs.find(_resolvedName);

      if (this->dataPtr->pluginToDlHandlePtrs.end() == it)
      {
        // LCOV_EXCL_START
        std::cerr << "[ignition::Loader::PrivateGetInfo] A resolved name ["
                  << _resolvedName << "] could not be found in the "
                  << "PluginToDlHandleMap. This should not be possible! Please "
                  << "report this bug!\n";
        assert(false);
        return nullptr;
        // LCOV_EXCL_STOP
      }

      return it->second;
    }

    /////////////////////////////////////////////////
    Loader::Implementation::Implementation()
    {
      this->StorePlugins(ExtractRegistry(kNativePlugins), nullptr);
    }

    /////////////////////////////////////////////////
    std::shared_ptr<void> Loader::Implementation::CreateDlHandle(
        const std::string &_full_path)
    {
      std::shared_ptr<void> dlHandlePtr;

      // Call dlerror() before dlopen(~) to ensure that we get accurate error
      // reporting afterwards. The function dlerror() is stateful, and that
      // state gets cleared each time it is called.
      dlerror();

      // NOTE: We open using RTLD_LOCAL instead of RTLD_GLOBAL to prevent the
      // symbols of different libraries from writing over each other.
      void *dlHandle = dlopen(_full_path.c_str(), RTLD_LAZY | RTLD_LOCAL);

      const char *loadError = dlerror();
      if (nullptr == dlHandle || nullptr != loadError)
      {
        std::cerr << "Error while loading the library [" << _full_path << "]: "
                  << loadError << std::endl;

        // Just return a nullptr if the library could not be loaded. The
        // Loader::LoadLib(~) function will handle this gracefully.
        return nullptr;
      }

      // The dl library maintains a reference count of how many times dlopen(~)
      // or dlclose(~) has been called on each loaded library. dlopen(~)
      // increments the reference count while dlclose(~) decrements the count.
      // When the count reaches zero, the operating system is free to unload
      // the library (which it might or might not choose to do; we do not seem
      // to have control over that, probably for the best).

      // The idea in our implementation here is to first check if the library
      // has already been loaded by this Loader. If it has been, then we
      // will see it in the dlHandlePtrMap, and we'll be able to get a shared
      // reference to its handle. The advantage of this is it allows us to keep
      // track of how many places the shared library is being used by plugins
      // that were generated from this Loader. When all of the instances
      // spawned by this Loader are gone, the destructor of the
      // std::shared_ptr will call dlclose on the library handle, bringing down
      // its dl library reference count.

      bool inserted;
      DlHandleMap::iterator it;
      std::tie(it, inserted) = this->dlHandlePtrMap.insert(
            std::make_pair(dlHandle, std::weak_ptr<void>()));

      if (!inserted)
      {
        // This shared library has already been loaded by this Loader in
        // the past, so we should use the reference counter that already
        // exists for it.
        dlHandlePtr = it->second.lock();

        if (dlHandlePtr)
        {
          // The reference counter of this library is still active.

          // The functions dlopen and dlclose keep their own independent
          // counters for how many times each of them has been called. The
          // library is unloaded once dlclose has been called as many times as
          // dlopen.
          //
          // At this line of code, we know that dlopen had been called by this
          // Loader instance prior to this run of LoadLib. Therefore,
          // we should undo the dlopen that we did just a moment ago in this
          // function so that only one dlclose must be performed to finally
          // close the shared library. That final dlclose will be performed in
          // the destructor of the std::shared_ptr<void> which stores the
          // library handle.
          dlclose(dlHandle);
        }
      }

      if (!dlHandlePtr)
      {
        // The library was not already loaded (or if it was loaded in the past,
        // it is no longer active), so we should create a reference counting
        // handle for it.
        dlHandlePtr = std::shared_ptr<void>(
              dlHandle, [](void *ptr) { dlclose(ptr); }); // NOLINT

        it->second = dlHandlePtr;
      }

      return dlHandlePtr;
    }

    /////////////////////////////////////////////////
    std::vector<ConstInfoPtr> Loader::Implementation::OldIgnitionHook(
        void *_infoFuncPtr,
        const std::string &_pathToLibrary) const
    {
      // TODO(anyone): Remove this entire function once all packages using
      // ignition-plugin have been re-built and re-released using the new hook
      // API.
      std::cerr << "The library [" << _pathToLibrary << "] is using a "
                << "deprecated method for registering plugins. Please "
                << "recompile this library with the newest version of "
                << "ignition-plugin at your earliest convenience.\n";

      std::vector<ConstInfoPtr> loadedPlugins;

      using PluginLoadFunctionSignature =
          void(*)(void * const, const void ** const,
                  int *, std::size_t *, std::size_t *);

      // Note: InfoHook (below) is a function with a signature that matches
      // PluginLoadFunctionSignature.
      auto InfoHook =
          reinterpret_cast<PluginLoadFunctionSignature>(_infoFuncPtr);

      // INFO_API_VERSION is a deprecated method for checking the API version
      // of the loaded plugin library.
      const int INFO_API_VERSION = 1;

      int version = INFO_API_VERSION;
      std::size_t size = sizeof(v1::Info);
      std::size_t alignment = alignof(v1::Info);

      const std::unordered_map<std::string, v1::Info> *allInfo = nullptr;

      // Note: static_cast cannot be used to convert from a T** to a void**
      // because of the possibility of breaking the type system by assigning a
      // Non-T pointer to the T* memory location. However, we need to retrieve
      // a reference to an STL-type using a C-compatible function signature, so
      // we resort to a reinterpret_cast to achieve this.
      //
      // Despite its many dangers, reinterpret_cast is well-defined for casting
      // between pointer types as of C++11, as explained in bullet point 1 of
      // the "Explanation" section in this reference:
      // http://en.cppreference.com/w/cpp/language/reinterpret_cast
      //
      // We have a tight grip over the implementation of how the `allInfo`
      // pointer gets used, so we do not need to worry about its memory address
      // being filled with a non-compatible type. The only risk would be if a
      // user decides to implement their own version of
      // IgnitionPluginHook, but they surely would have no
      // incentive in doing that.
      //
      // Also note that the main reason we jump through these hoops is in order
      // to safely support plugin libraries on Windows that choose to compile
      // against the static runtime. Using this pointer-to-a-pointer approach is
      // the cleanest way to ensure that all dynamically allocated objects are
      // deleted in the same heap that they were allocated from.
      InfoHook(nullptr, reinterpret_cast<const void**>(&allInfo),
           &version, &size, &alignment);

      if (INFO_API_VERSION != version)
      {
        std::cerr << "The library [" << _pathToLibrary << "] is using an "
                  << "impossible version [" << version << "] of the deprecated "
                  << "IgnitionPluginHook API.\n";
        return loadedPlugins;
      }

      if (sizeof(v1::Info) != size || alignof(v1::Info) != alignment)
      {
        std::cerr << "The plugin::Info size or alignment are not consistent "
               << "with the expected values for the library [" << _pathToLibrary
               << "]:\n -- size: expected " << sizeof(v1::Info)
               << " | received " << size << "\n -- alignment: expected "
               << alignof(v1::Info) << " | received " << alignment << "\n"
               << " -- We will not be able to safely load plugins from that "
               << "library.\n";

        return loadedPlugins;
      }

      if (!allInfo)
      {
        std::cerr << "The library [" << _pathToLibrary << "] failed to provide "
                  << "ignition::plugin Info for unknown reasons. Please report "
                  << "this error as a bug!\n";

        return loadedPlugins;
      }

      for (const auto &entry : *allInfo)
      {
        loadedPlugins.push_back(
              std::make_shared<info_v1::Info>(v1::Update(entry.second)));
      }

      return loadedPlugins;
    }

    /////////////////////////////////////////////////
    std::vector<ConstInfoPtr> Loader::Implementation::ReceivePlugins(
        const std::shared_ptr<void> &_dlHandle,
        const std::string& _pathToLibrary) const
    {
      std::vector<ConstInfoPtr> loadedPlugins;

      // This function should never be called with a nullptr _dlHandle
      assert(_dlHandle &&
             "Bug in code: Loader::Implementation::ReceivePlugins "
             "was called with a nullptr value for _dlHandle.");

      const auto archiveIt = kArchive.dlHandleToInfo.find(_dlHandle.get());
      if (archiveIt != kArchive.dlHandleToInfo.end())
      {
        for (const auto &weakInfo : archiveIt->second)
        {
          const auto info = weakInfo.lock();
          if (info)
          {
            loadedPlugins.push_back(info);
          }
          else
          {
            std::cerr << "[ignition::plugin::Loader::LoadLib] Error: Failed "
                      << "to lock an archived ConstInfoPtr for ["
                      << _pathToLibrary << "]. This should never happen! "
                      << "Please report this bug!\n";
          }
        }

        return loadedPlugins;
      }

      // Receive old-fashioned (deprecated) plugin info
      const std::string infoSymbol = "IgnitionPluginHook";
      void *infoFuncPtr = dlsym(_dlHandle.get(), infoSymbol.c_str());
      if (infoFuncPtr)
        loadedPlugins = OldIgnitionHook(infoFuncPtr, _pathToLibrary);

      // Receive plugin info using the newer method
      const std::vector<ConstInfoPtr> registryInfo =
          ExtractRegistry(kDynamicPlugins);

      loadedPlugins.insert(loadedPlugins.end(),
                           registryInfo.begin(), registryInfo.end());

      // Add this newly registered plugin info to the archive so that other
      // Loaders can access it.
      ArchivePluginInfo(loadedPlugins, _dlHandle);

      return loadedPlugins;
    }

    /////////////////////////////////////////////////
    std::unordered_set<std::string> Loader::Implementation::StorePlugins(
        std::vector<ConstInfoPtr> _loadedPlugins,
        const std::shared_ptr<void> _dlHandle)
    {
      std::unordered_set<std::string> newPlugins;

      for (ConstInfoPtr &plugin : _loadedPlugins)
      {
        // Add the plugin's aliases to the alias map
        for (const std::string &alias : plugin->aliases)
          this->aliases[alias].insert(plugin->name);

        // Add the plugin to the map
        this->plugins.insert(std::make_pair(plugin->name, plugin));

        // Add the plugin's name to the set of newPlugins
        newPlugins.insert(plugin->name);

        // Save the dl handle for this plugin
        this->pluginToDlHandlePtrs[plugin->name] = _dlHandle;
      }

      this->dlHandleToPluginMap[_dlHandle.get()] = newPlugins;

      return newPlugins;
    }

    /////////////////////////////////////////////////
    std::string Loader::Implementation::LookupPlugin(
        const std::string &_nameOrAlias) const
    {
      const PluginMap::const_iterator name = this->plugins.find(_nameOrAlias);

      if (this->plugins.end() != name)
        return _nameOrAlias;

      const AliasMap::const_iterator alias = this->aliases.find(_nameOrAlias);
      if (this->aliases.end() != alias && !alias->second.empty())
      {
        if (alias->second.size() == 1)
          return *alias->second.begin();

        // We use a stringstream because we're going to output to std::cerr, and
        // we want it all to print at once, but std::cerr does not support
        // buffering.
        std::stringstream ss;

        ss << "[ignition::plugin::Loader::LookupPlugin] Failed to resolve the "
           << "alias [" << _nameOrAlias << "] because it refers to multiple "
           << "plugins:\n";
        for (const std::string &plugin : alias->second)
          ss << " -- [" << plugin << "]\n";

        std::cerr << ss.str();

        return "";
      }

      std::cerr << "[ignition::plugin::Loader::LookupPlugin] Failed to get "
                << "info for [" << _nameOrAlias << "]. Could not find a plugin "
                << "with that name or alias.\n";

      return "";
    }

    /////////////////////////////////////////////////
    bool Loader::Implementation::ForgetLibrary(void *_dlHandle)
    {
      if (_dlHandle == nullptr)
      {
        // If _dlHandle is a nullptr, that means the user asked to forget the
        // library of a native plugin, but that is impossible because native
        // plugins are tied to the application itself and cannot be unloaded.

        // TODO(anyone): Should we print a warning here?
        return false;
      }

      DlHandleToPluginMap::iterator it = dlHandleToPluginMap.find(_dlHandle);
      if (dlHandleToPluginMap.end() == it)
        return false;

      const std::unordered_set<std::string> &forgottenPlugins = it->second;

      for (const std::string &forget : forgottenPlugins)
      {
        // Erase each alias entry corresponding to this plugin
        const ConstInfoPtr &info = plugins.at(forget);
        for (const std::string &alias : info->aliases)
          this->aliases.at(alias).erase(info->name);
      }

      for (const std::string &forget : forgottenPlugins)
      {
        // CRUCIAL DEV NOTE (MXG): Be sure to erase the Info from
        // `plugins` BEFORE erasing the plugin entry in `pluginToDlHandlePtrs`,
        // because the Info structs require the library to remain loaded
        // for the destructors of their `deleter` member variables.

        // This erase should come FIRST.
        plugins.erase(forget);

        // This erase should come LAST.
        pluginToDlHandlePtrs.erase(forget);
      }

      // Dev note (MXG): We do not need to delete anything from `dlHandlePtrMap`
      // because it uses std::weak_ptrs. It will clear itself automatically.

      // Dev note (MXG): This erase call should come at the very end of this
      // function to ensure that the `forgottenPlugins` reference remains valid
      // while it is being used.
      dlHandleToPluginMap.erase(it);

      // Dev note (MXG): We do not need to call dlclose because that will be
      // taken care of automatically by the std::shared_ptr that manages the
      // shared library handle.

      return true;
    }

    namespace detail
    {
    std::shared_ptr<const void> IgnitionPluginHook_v1(
        const info_v1::Info &_inputInfo,
        const std::size_t _inputInfoSize,
        const std::size_t _inputInfoAlign)
    {
      if (sizeof(info_v1::Info) != _inputInfoSize
          || alignof(info_v1::Info) != _inputInfoAlign)
      {
        std::cerr << "The ignition::plugin::info_v1::Info size or alignment "
                  << "are not consistent with the expected values for the "
                  << "plugin named [" << _inputInfo.name << "]:\n"
                  << " -- size: expected " << sizeof(info_v1::Info)
                  << " | received " << _inputInfoSize << "\n"
                  << " -- alignment: expected " << alignof(info_v1::Info)
                  << " | received " << _inputInfoAlign << "\n";

        registrationOkay = false;
        return nullptr;
      }

      Registry &registry =
          registeringDynamicPlugins? kDynamicPlugins : kNativePlugins;

      InfoMap::iterator it;
      bool inserted;

      // Attempt to insert a dummy nullptr entry
      std::tie(it, inserted) =
          registry.pluginMap.insert(std::make_pair(_inputInfo.symbol, nullptr));

      ignition::plugin::info_v1::InfoPtr &infoPtr = it->second;

      if (inserted)
      {
        // If the dummy nullptr was inserted, then we should replace it with the
        // info that was actually passed in by constructing a new InfoPtr for
        // it.
        infoPtr = std::make_shared<info_v1::Info>(_inputInfo);
        infoPtr->name = DemangleSymbol(infoPtr->symbol);

        for (const auto &interface : _inputInfo.interfaces)
          infoPtr->demangledInterfaces.insert(DemangleSymbol(interface.first));
      }
      else
      {
        // If the dummy nullptr was not inserted, then an entry already existed
        // for this plugin type. We should still insert each of the interface
        // map entries and aliases provided by the input info, just in case any
        // of them are missing from the currently existing entry. This allows
        // the user to specify different interfaces and aliases for the same
        // plugin type using different macros in different locations or across
        // multiple translation units.

        for (const auto &interface : _inputInfo.interfaces)
        {
          infoPtr->interfaces.insert(interface);
          infoPtr->demangledInterfaces.insert(DemangleSymbol(interface.first));
        }

        for (const auto &aliasSetEntry : _inputInfo.aliases)
          infoPtr->aliases.insert(aliasSetEntry);
      }
      return infoPtr;
    }

    void IgnitionPluginHookCleanup_v1(
        std::shared_ptr<const void> &_handle)
    {
      const auto dlHandleIt = kArchive.infoToDlHandle.find(_handle.get());
      if (dlHandleIt != kArchive.infoToDlHandle.end())
      {
        kArchive.dlHandleToInfo.erase(dlHandleIt->second);
        kArchive.infoToDlHandle.erase(dlHandleIt);
      }

      // We should reset this shared_ptr from the ignition-plugin-loader library
      // because this is the library that instantiated it. In MSVC, each DLL
      // potentially has its own allocation heap, so it's safest to delete it
      // from the same library that allocated it.
      _handle.reset();
    }
    }
  }
}
