/*!
 *  Copyright (c) 2017 by Contributors
 * \file dso_module.cc
 * \brief Module to load from dynamic shared library.
 */
#include <dmlc/memory_io.h>
#include <tvm/runtime/module.h>
#include <tvm/runtime/registry.h>
#include <tvm/runtime/packed_func.h>
#include "./meta_data.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tvm {
namespace runtime {

// Module to load from dynamic shared libary.
// This is the default module TVM used for hostside AOT
class DSOModuleNode final : public ModuleNode {
 public:
  ~DSOModuleNode() {
    if (lib_handle_) Unload();
  }

  const char* type_key() const final {
    return "dso";
  }

  PackedFunc GetFunction(
      const std::string& name,
      const std::shared_ptr<ModuleNode>& sptr_to_self) final {
    BackendPackedCFunc faddr = GetFuncPtr(name);
    if (faddr == nullptr) return PackedFunc();
    return PackedFunc([faddr, sptr_to_self](TVMArgs args, TVMRetValue* rv) {
        int ret = (*faddr)(
            (void*)args.values, // NOLINT(*)
            (int*)args.type_codes, // NOLINT(*)
            args.num_args);
        CHECK_EQ(ret, 0) << TVMGetLastError();
      });
  }

  void Init(const std::string& name) {
    Load(name);
    CHECK(lib_handle_ != nullptr)
        << "Failed to load dynamic shared library " << name;
    void** ctx_addr =
        reinterpret_cast<void**>(
            GetGlobalVPtr(runtime::symbol::tvm_module_ctx));
    if (ctx_addr != nullptr) {
      *ctx_addr = this;
    }
    // Load the imported modules
    const char* dev_mblob =
        reinterpret_cast<const char*>(
            GetGlobalVPtr(runtime::symbol::tvm_dev_mblob));
    const unsigned long* dev_mblob_nbytes =   // NOLINT(*)
        reinterpret_cast<const unsigned long*>(  // NOLINT(*)
            GetGlobalVPtr(runtime::symbol::tvm_dev_mblob_nbytes));

    if (dev_mblob != nullptr) {
      CHECK(dev_mblob_nbytes != nullptr);
      dmlc::MemoryFixedSizeStream fs(
          (void*)dev_mblob, dev_mblob_nbytes[0]);  // NOLINT(*)
      dmlc::Stream* stream = &fs;
      uint64_t size;
      CHECK(stream->Read(&size));
      for (uint64_t i = 0; i < size; ++i) {
        std::string tkey;
        CHECK(stream->Read(&tkey));
        std::string fkey = "module.loadbinary_" + tkey;
        const PackedFunc* f = Registry::Get(fkey);
        CHECK(f != nullptr)
            << "Loader of " << tkey << "("
            << fkey << ") is not presented.";
        Module m = (*f)(static_cast<void*>(stream));
        this->imports_.push_back(m);
      }
    }
  }

 private:
  BackendPackedCFunc GetFuncPtr(const std::string& name) {
    if (name == runtime::symbol::tvm_module_main) {
      const char* entry_name = reinterpret_cast<const char*>(
          GetGlobalVPtr(runtime::symbol::tvm_module_main));
      CHECK(entry_name!= nullptr)
          << "Symbol " << runtime::symbol::tvm_module_main << " is not presented";
      return GetFuncPtr_(entry_name);
    } else {
      return GetFuncPtr_(name);
    }
  }
  // Platform dependent handling.
#if defined(_WIN32)
  // library handle
  HMODULE lib_handle_{nullptr};
  // Load the library
  void Load(const std::string& name) {
    // use wstring version that is needed by LLVM.
    std::wstring wname(name.begin(), name.end());
    lib_handle_ = LoadLibraryW(wname.c_str());
  }
  BackendPackedCFunc GetFuncPtr_(const std::string& name) {
    return reinterpret_cast<BackendPackedCFunc>(
        GetProcAddress(lib_handle_, (LPCSTR)name.c_str()));  // NOLINT(*)
  }
  void* GetGlobalVPtr(const std::string& name) {
    return reinterpret_cast<void*>(
        GetProcAddress(lib_handle_, name.c_str())); // NOLINT(*)
  }
  void Unload() {
    FreeLibrary(lib_handle_);
  }
#else
  // Library handle
  void* lib_handle_{nullptr};
  // load the library
  void Load(const std::string& name) {
    lib_handle_ = dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
  }
  BackendPackedCFunc GetFuncPtr_(const std::string& name) {
    return reinterpret_cast<BackendPackedCFunc>(
        dlsym(lib_handle_, name.c_str()));
  }
  void* GetGlobalVPtr(const std::string& name) {
    return dlsym(lib_handle_, name.c_str());
  }
  void Unload() {
    dlclose(lib_handle_);
  }
#endif
};

TVM_REGISTER_GLOBAL("module.loadfile_so")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    std::shared_ptr<DSOModuleNode> n = std::make_shared<DSOModuleNode>();
    n->Init(args[0]);
    *rv = runtime::Module(n);
  });
}  // namespace runtime
}  // namespace tvm
