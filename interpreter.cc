#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <exception>
#include <sys/mman.h>
#include <unistd.h>
#include <cmath>

/**
 * Some limitations of this program:
 * 
 * 1. No exception-handling support；
 * 2. No thread-safe guaranteed;
 * 3. The amount of consecutive "+", "-", "<" and ">" showing up in the source code cannot exceed 255;
 * 4. Only support macOS 64bit.
 * 
 */

// #define ENABLE_DEBUG

constexpr size_t TAPE_SIZE = 30000;
constexpr size_t MAX_NESTING = 100;

#ifdef ENABLE_DEBUG
template<typename T>
void debugVec(std::vector<T> *vp) {
  for (auto i = vp->begin(); i != vp->end(); ++i) {
    std::cout << std::hex << static_cast<size_t>(*i) << std::endl;
  }
}
void debugTape(unsigned char *arr, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    std::cout << static_cast<int>(arr[i]);
  }
}
#endif

// allocate executable memory.
void setupExecutableMem(std::vector<uint8_t>* machineCode) {
  // get page size in bytes.
  auto pageSize = getpagesize();
  auto *mem = static_cast<uint8_t*>(mmap(
    NULL, 
    static_cast<size_t>(std::ceil(machineCode->size() / static_cast<double>(pageSize))), 
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, 
    -1, 
    0));
  if (mem == MAP_FAILED) {
    std::cerr << "[error] Can't allocate memory.\n"; 
    std::exit(1);
  }
  for (size_t i = 0; i < machineCode->size(); ++i) {
    mem[i] = machineCode->at(i);
  }

  // save the current %rip on stack (by PC-relative).
  asm(R"(
    lea 0x7(%%rip), %%rax 
    pushq %%rax
    movq %0, %%rax
    jmpq *%%rax
  )":: "m" (mem));
}

// abstract machine model.
struct bfState {
  unsigned char tape[TAPE_SIZE] = {0};
  unsigned char* ptr = nullptr;
  bfState() {
    ptr = tape;
  }
};

void bfJITCompile(std::vector<char>* program, bfState* state) {
  // helpers.
  auto _appendBytecode = [](auto& byteCode, auto& machineCode) {
    machineCode.insert(machineCode.end(), byteCode.begin(), byteCode.end());
  };

  auto _resolvePtrAddr = [](auto ptrAddr) -> auto {
    return std::vector<uint8_t> {
      static_cast<uint8_t>(ptrAddr & 0xff),
      static_cast<uint8_t>((ptrAddr & 0xff00) >> 8),
      static_cast<uint8_t>((ptrAddr & 0xff0000) >> 16),
      static_cast<uint8_t>((ptrAddr & 0xff000000) >> 24), 
      static_cast<uint8_t>((ptrAddr & 0xff00000000) >> 32),
      static_cast<uint8_t>((ptrAddr & 0xff0000000000) >> 40),
      static_cast<uint8_t>((ptrAddr & 0xff000000000000) >> 48),
      static_cast<uint8_t>((ptrAddr & 0xff00000000000000) >> 56), 
    };
  };

  auto _resolveAddrDiff = [](auto addrDiff) -> auto {
    return std::vector<uint8_t> {
      static_cast<uint8_t>(addrDiff & 0xff),
      static_cast<uint8_t>((addrDiff & 0xff00) >> 8),
      static_cast<uint8_t>((addrDiff & 0xff0000) >> 16),
      static_cast<uint8_t>((addrDiff & 0xff000000) >> 24),
    };
  };

  std::vector<uint8_t> machineCode {
    0x48, 0xbb, /* mem slot */
  };
  std::vector<size_t> jmpLocIndex {};

  // save the base pointer in %rbx.
  auto basePtrBytes = _resolvePtrAddr(reinterpret_cast<size_t>(state->ptr));
  machineCode.insert(machineCode.begin() + 2, basePtrBytes.begin(), basePtrBytes.end());
  
  // codegen.
  for (auto tok = program->cbegin(); tok != program->cend(); ++tok) {
    size_t n = 0;
    auto ptrAddr = reinterpret_cast<size_t>(state->ptr);

    switch(*tok) {
      case '+': {
        /**
          addb $0x1, (%rbx)
         */
        for (n = 0; *tok == '+'; ++n, ++tok);
        const auto ptrBytes = _resolvePtrAddr(ptrAddr);
        std::vector<uint8_t> byteCode { 
          0x80, 0x3, static_cast<uint8_t>(n),
        };
        _appendBytecode(byteCode, machineCode);
        --tok;
        break;
      } 
      case '-': {
        /**
          subb $0x1, (%rbx)
         */
        for (n = 0; *tok == '-'; ++n, ++tok);
        const auto ptrBytes = _resolvePtrAddr(ptrAddr);
        std::vector<uint8_t> byteCode { 
          0x80, 0x2b, static_cast<uint8_t>(n),
        };
        _appendBytecode(byteCode, machineCode);
        --tok;
        break;
      }
      case '>': {
        /**
          add $0x1, %rbx
         */
        for (n = 0; *tok == '>'; ++n, ++tok);
        std::vector<uint8_t> byteCode { 
          0x48, 0x83, 0xc3, static_cast<uint8_t>(n),
        };
        _appendBytecode(byteCode, machineCode);
        --tok;  // counteract the tok++ in the main loop.
        break;
      }
      case '<': {
        /**
          sub $0x1, %rbx
         */
        for (n = 0; *tok == '<'; ++n, ++tok);
        std::vector<uint8_t> byteCode { 
          0x48, 0x83, 0xeb, static_cast<uint8_t>(n),
        };
        _appendBytecode(byteCode, machineCode);
        --tok;  // counteract the tok++ in the main loop.
        break;
      }
      case ',': {
        std::vector<uint8_t> byteCode { 
          0xb8, 0x3, 0x0, 0x0, 0x2,
          0xbf, 0x0, 0x0, 0x0, 0x0,
          0x48, 0x89, 0xde,
          0xba, 0x1, 0x0, 0x0, 0x0,
          0xf, 0x5,
        };
        _appendBytecode(byteCode, machineCode);
        break;
      }
      case '.': {
        std::vector<uint8_t> byteCode { 
          0xb8, 0x4, 0x0, 0x0, 0x2,
          0xbf, 0x1, 0x0, 0x0, 0x0,
          0x48, 0x89, 0xde,
          0xba, 0x1, 0x0, 0x0, 0x0,
          0xf, 0x5,
        };
        _appendBytecode(byteCode, machineCode);
        break;
      }
      case '[': {
        std::vector<uint8_t> byteCode { 
          0x80, 0x3b, 0x0,
          0xf, 0x84, 0x0, 0x0, 0x0, 0x0, /* near jmp */
        };
        // record the jump relocation pos.
        _appendBytecode(byteCode, machineCode);
        jmpLocIndex.push_back(machineCode.size());
        break;
      }
      case ']': {
        std::vector<uint8_t> byteCode { 
          0x80, 0x3b, 0x0,
          0xf, 0x85, 0x0, 0x0, 0x0, 0x0, /* near jmp */
        };
        _appendBytecode(byteCode, machineCode);
        // calculate real offset.
        auto bDiff = _resolveAddrDiff(static_cast<uint32_t>(jmpLocIndex.back() - machineCode.size()));
        auto fDiff = _resolveAddrDiff(static_cast<uint32_t>(machineCode.size() - jmpLocIndex.back()));

        // relocate the memory address of the generated machine code.
        machineCode.erase(machineCode.end() - 4, machineCode.end());
        machineCode.insert(machineCode.end(), bDiff.begin(), bDiff.end());
        
        // relocate the corresponding previous "[".
        machineCode.erase(machineCode.begin() + jmpLocIndex.back() - 4, machineCode.begin() + jmpLocIndex.back());
        machineCode.insert(machineCode.begin() + jmpLocIndex.back() - 4, fDiff.begin(), fDiff.end());
        jmpLocIndex.pop_back();

        // reduce unnecessary cmp.
        auto ctok = tok + 1;
        for (n = 0; *ctok == ']'; ++n, ++ctok);
        if (n > 0) {
          std::vector<uint8_t> byteCode {
            0xeb, static_cast<uint8_t>(n * 11 - 2),
          };
          _appendBytecode(byteCode, machineCode);
        }
        break;
      }
    }
  }

  // add epilogue.
  /**
    pop %rax
    jmpq %rax  # return to normal C++ execution.
   */
  machineCode.push_back(0x58);
  machineCode.push_back(0xff);
  machineCode.push_back(0xe0);

  // dynamic execution.
  setupExecutableMem(&machineCode);
}

void bfInterpret(const char* program, bfState* state) {
  const char* loops[MAX_NESTING];
  auto nloops = 0;
  auto nskip = 0;
  size_t n = 0;
  
  while(true) {
    // switch threading.
    switch(*program++) {
      case '<': {
        for (n = 1; *program == '<'; ++n, ++program);
        if (!nskip) state->ptr -= n;
        break;
      }
      case '>': {
        for (n = 1; *program == '>'; ++n, ++program);
        if (!nskip) state->ptr += n;
        break; 
      }
      case '+': {
        for (n = 1; *program == '+'; ++n, ++program);
        if (!nskip) *state->ptr += n;
        break;
      }
      case '-': {
        for (n = 1; *program == '-'; ++n, ++program);
        if (!nskip) *state->ptr -= n;
        break;
      }
      case ',': {
        if (!nskip) *state->ptr = static_cast<unsigned char>(std::getchar());
        break;
      }
      case '.': {
        if (!nskip) 
          std::cout << *state->ptr;
        break;
      }
      case '[': {
        if (nloops == MAX_NESTING) std::terminate();
        loops[nloops++] = program;
        if (!*state->ptr) ++nskip;
        break;
      }
      case ']': {
        if (nloops == 0) std::terminate();
        if (*state->ptr) program = loops[nloops - 1];
        else --nloops;
        if (nskip) --nskip;
        break;
      }
      case ' ': {
        for (n = 1; *program == ' '; ++n, ++program);  // clear spaces.
        break;
      }
      case '\0': {
        return;
      }
    }
  }
}

inline void bfRunDefault(const char* sourceCode) {
  bfState bfs;
  bfInterpret(sourceCode, &bfs);
}

inline void bfRunJIT(std::vector<char>* sourceCode) {
  bfState bfs;
  bfJITCompile(sourceCode, &bfs);
}

int main(int argc, char** argv) {
  char token;
  std::vector<char> v {};
  if (argc > 1) {
    std::string inputSourceFileName = std::string(*(argv + 1));
    std::ifstream f(inputSourceFileName, std::ios::binary);
    while (f.is_open() && f.good() && f >> token) {
      v.push_back(token);
    }
  }
  if (v.size() > 0) {
    if (argc > 2 && std::string(*(argv + 2)) == "--jit") {
      bfRunJIT(&v);
    } else {
      bfRunDefault(v.data());
    }
  }
  return 0;
}
