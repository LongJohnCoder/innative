// Copyright �2018 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in native-wasm.h

#include "native-wasm/export.h"
#include <iostream>
#include <filesystem>

using namespace std::filesystem;

int main(int argc, char *argv[])
{
  std::cout << "Native-wasm v" << NATIVE_WASM_VERSION_MAJOR << "." << NATIVE_WASM_VERSION_MINOR << "." << NATIVE_WASM_VERSION_REVISION << " Test Utility" << std::endl;
  std::cout << std::endl;

  path testdir("../spec/test/core");
  std::vector<path> testfiles;

  for(auto& p : recursive_directory_iterator(testdir, directory_options::skip_permission_denied))
  {
    if(!STRICMP(p.path().extension().u8string().data(), ".wast"))
      testfiles.push_back(p.path());
  }

  std::cout << "Running through " << testfiles.size() << " official webassembly spec tests." << std::endl;
  
  for(auto file : testfiles)
  {
    ValidationError* errors = 0;
    int r = native_wasm_compile_script(file.generic_u8string().data(), ENV_DEBUG | ENV_STRICT, &errors);
    if(r)
      std::cout << "Error running script " << file << ": " << r << std::endl;
    if(!errors)
      std::cout << file << ": SUCCESS" << std::endl;
    else
    {
      std::cout << file << ": FAILED" << std::endl;
      while(errors != 0)
      {
        std::cout << "  " << errors->error << std::endl;
        errors = errors->next;
      }
      std::cout << std::endl;
    }
  }
  
  char c;
  std::cin >> c;
  return 0;
}