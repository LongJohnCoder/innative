// Copyright (c)2019 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in innative.h

#include "parse.h"
#include "validate.h"
#include "compile.h"
#include "tools.h"
#include "wat.h"
#include <atomic>
#include <thread>
#include <stdio.h>

using namespace innative;
using namespace utility;

Environment* innative::CreateEnvironment(unsigned int modules, unsigned int maxthreads, const char* arg0)
{
  Environment* env = (Environment*)calloc(1, sizeof(Environment));
  if(env)
  {
    env->modulemap = kh_init_modules();
    env->whitelist = kh_init_modulepair();
    env->cimports = kh_init_cimport();
    env->modules = trealloc<Module>(0, modules);
    env->alloc = new __WASM_ALLOCATOR();

    if(!env->modules)
    {
      free(env);
      return nullptr;
    }

    env->capacity = modules;
    env->flags = ENV_SANDBOX;
    env->optimize = ENV_OPTIMIZE_O3;
    env->features = ENV_FEATURE_ALL;
    env->maxthreads = maxthreads;
    env->linker = 0;
    env->log = stdout;
    env->loglevel = LOG_WARNING;
    env->libpath = utility::AllocString(*env, GetProgramPath(arg0).BaseDir().Get());
    env->objpath = 0;
    env->system = "";
    env->wasthook = 0;
  }
  return env;
}

void innative::ClearEnvironmentCache(Environment* env, Module* m)
{
  assert(env != nullptr);

  if(m)
    DeleteCache(*env, *m);
  else
    DeleteContext(*env, false); // We can't actually shutdown LLVM here because that permanently shuts it down and there is no way to restore it.
}

void innative::DestroyEnvironment(Environment* env)
{
  if(!env)
    return;

  ClearEnvironmentCache(env, 0);

  for(varuint32 i = 0; i < env->n_modules; ++i)
  {
    kh_destroy_exports(env->modules[i].exports);
    if(env->modules[i].importsection.imports)
      free(env->modules[i].importsection.imports);
    assert(!env->modules[i].cache);
  }

  delete env->alloc;
  kh_destroy_modulepair(env->whitelist);
  kh_destroy_modules(env->modulemap);
  kh_destroy_cimport(env->cimports);
  free(env->modules);
  free(env);
}

void innative::LoadModule(Environment* env, size_t index, const void* data, uint64_t size, const char* name, const char* path, int* err)
{
  Stream s = { (uint8_t*)data, size, 0 };
  std::string fallback;
  if(!name)
  {
    fallback = "m" + std::to_string(index);
    name = fallback.data();
  }

  if((env->flags & ENV_ENABLE_WAT) && size > 0 && s.data[0] != 0)
  {
    env->modules[index] = { 0 };
    *err = innative::ParseWatModule(*env, env->modules[index], s.data, size, StringRef{ name, strlen(name) });
  }
  else
    *err = ParseModule(s, *env, env->modules[index], ByteArray((uint8_t*)name, (varuint32)strlen(name)), env->errors);

  env->modules[index].path = path;
  ((std::atomic<size_t>&)env->n_modules).fetch_add(1, std::memory_order_release);
}

void innative::AddModule(Environment* env, const void* data, uint64_t size, const char* name, int* err)
{
  if(!env || !err)
  {
    *err = ERR_FATAL_NULL_POINTER;
    return;
  }

  const char* path = nullptr;
  std::unique_ptr<uint8_t[]> data_module;
  if(!size)
  {
    long sz = 0;
    path = (const char*)data;
    data_module = utility::LoadFile(path, sz);
    if(data_module.get() == nullptr)
    {
      *err = ERR_FATAL_FILE_ERROR;
      return;
    }
    size = sz;
    data = data_module.get();
  }

  if((env->flags & ENV_MULTITHREADED) != 0 && env->maxthreads > 0)
  {
    while(((std::atomic<size_t>&)env->size).load(std::memory_order_relaxed) - ((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed) >= env->maxthreads)
      std::this_thread::sleep_for(std::chrono::milliseconds(2)); // If we're using maxthreads, block until one finishes (we must block up here or we risk a deadlock)
  }

  size_t index = ((std::atomic<size_t>&)env->size).fetch_add(1, std::memory_order_acq_rel);
  if(index >= ((std::atomic<size_t>&)env->capacity).load(std::memory_order_acquire))
  {
    while(((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed) != index)
      std::this_thread::sleep_for(std::chrono::milliseconds(5)); // If we've exceeded our capacity, block until all other threads have finished

    env->modules = trealloc<Module>(env->modules, index * 2);
    if(!env->modules)
    {
      *err = ERR_FATAL_OUT_OF_MEMORY;
      return;
    }
    ((std::atomic<size_t>&)env->capacity).store(index * 2, std::memory_order_release);
  }

  if(env->flags & ENV_MULTITHREADED)
    std::thread(LoadModule, env, index, data, size, name, path, err).detach();
  else
    LoadModule(env, index, data, size, name, path, err);
}

void innative::AddWhitelist(Environment* env, const char* module_name, const char* export_name)
{
  if(!module_name || !export_name)
    return;

  auto whitelist = tmalloc<char>(*env, CanonWhitelist(module_name, export_name, nullptr));
  CanonWhitelist(module_name, export_name, whitelist);

  int r;
  auto iter = kh_put_modulepair(env->whitelist, whitelist, &r);
  //kh_val(env->whitelist, iter) = !ftype ? FunctionType{ TE_NONE, 0, 0, 0, 0 } : *ftype;
}

enum IN_ERROR innative::AddEmbedding(Environment* env, int tag, const void* data, uint64_t size)
{
  if(!env)
    return ERR_FATAL_NULL_POINTER;

  Embedding* embed = tmalloc<Embedding>(*env, 1);
  embed->tag = tag;
  embed->data = data;
  embed->size = size;
  embed->next = env->embeddings;
  env->embeddings = embed;

  return ERR_SUCCESS;
}

enum IN_ERROR innative::FinalizeEnvironment(Environment* env)
{
  // If we have an empty whitelist defined, all C function imports are illegal, so don't bother dumping symbols
  if(env->cimports && (!(env->flags & ENV_WHITELIST) || kh_size(env->whitelist) > 0))
  {
    for(Embedding* embed = env->embeddings; embed != nullptr; embed = embed->next)
    {
      Path path(env->libpath);
      std::string tmp;
      const char* src = (const char*)embed->data;

      if(embed->size)
      {
        // TODO: Allow GetSymbols to take a raw memory pointer to avoid this
        tmp = std::to_string((size_t)embed->data) + ".tmp";
        src = tmp.c_str();
        FILE* f;
        FOPEN(f, src, "wb");
        if(!f)
          return ERR_FATAL_FILE_ERROR;
        fwrite(embed->data, 1, embed->size, f);
        fclose(f);
      }

      {
        path.Append(src);

        FILE* f;
        FOPEN(f, path.c_str(), "rb");
        if(!f)
          path = src;

        FOPEN(f, path.c_str(), "rb");
#ifdef IN_PLATFORM_POSIX
        if(!f)
        {
          path = Path("/usr/lib/") + (const char*)embed->data;
          char* tmp = tmalloc<char>(*env, path.Get().size() + 1);
          tmemcpy<char>(tmp, path.Get().size() + 1, path.c_str(), path.Get().size() + 1);
          embed->data = tmp;
        }

        FOPEN(f, path.c_str(), "rb");
#endif
        if(!f)
        {
          fprintf(env->log, "Error loading file: %s\n", path.c_str());
          return ERR_FATAL_FILE_ERROR;
        }
        fclose(f);
      }

      auto symbols = GetSymbols(path.c_str(), env->log);
      if(!tmp.empty())
        std::remove(tmp.c_str());

      int r;
      for(auto symbol : symbols)
      {
        Identifier id;
        id.resize(symbol.size(), true, *env);
        memcpy(id.get(), symbol.data(), symbol.size());
        kh_put_cimport(env->cimports, id, &r);
        // On windows, because .lib files map to DLLs, they can have duplicate symbols from the dependent DLLs
        // that the DLL itself depends on. As a result, we cannot enforce this check until the linker resolves the symbols.
#ifndef IN_PLATFORM_WIN32
        if(!r)
          return ERR_INVALID_EMBEDDING;
#endif
      }
    }
  }

  while(((std::atomic<size_t>&)env->size).load(std::memory_order_relaxed) > ((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Spin until all modules have loaded

  return ERR_SUCCESS;
}

enum IN_ERROR innative::Compile(Environment* env, const char* file)
{
  if(!env)
    return ERR_FATAL_NULL_POINTER;

  // Before validating, add all modules to the modulemap. We must do this outside of LoadModule for multithreading reasons.
  for(size_t i = 0; i < env->n_modules; ++i)
  {
    int r;
    khiter_t iter = kh_put_modules(env->modulemap, env->modules[i].name, &r);
    if(!r)
      return ERR_FATAL_DUPLICATE_MODULE_NAME;
    kh_val(env->modulemap, iter) = i;
  }

  ValidateEnvironment(*env);
  if(env->errors)
  {
    internal::ReverseErrorList(env->errors); // Reverse error list so it appears in chronological order
    return ERR_VALIDATION_ERROR;
  }

  return CompileEnvironment(env, file);
}
IN_Entrypoint innative::LoadFunction(void* assembly, const char* module_name, const char* function)
{
  return (IN_Entrypoint)LoadDLLFunction(assembly, !function ? IN_INIT_FUNCTION : utility::CanonicalName(StringRef::From(module_name), StringRef::From(function)).c_str());
}

struct IN_TABLE
{
  IN_Entrypoint func;
  varuint32 type;
};

IN_Entrypoint innative::LoadTable(void* assembly, const char* module_name, const char* table, varuint32 index)
{
  IN_TABLE* ref = (IN_TABLE*)LoadDLLFunction(assembly, utility::CanonicalName(StringRef::From(module_name), StringRef::From(table)).c_str());
  return !ref ? nullptr : ref[index].func;
}

IRGlobal* innative::LoadGlobal(void* assembly, const char* module_name, const char* export_name)
{
  return (IRGlobal*)LoadDLLFunction(assembly, utility::CanonicalName(StringRef::From(module_name), StringRef::From(export_name)).c_str());
}

void* innative::LoadAssembly(const char* file)
{
  if(!file)
    return 0;

  Path path(file);
  
  return path.IsAbsolute() ? LoadDLL(path.c_str()) : LoadDLL((GetWorkingDir() + path).c_str());
}

void innative::FreeAssembly(void* assembly)
{
  FreeDLL(assembly);
}