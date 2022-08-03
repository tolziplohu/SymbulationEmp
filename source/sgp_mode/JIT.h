#ifndef SGP_JIT_H
#define SGP_JIT_H

// These need to be included for Organism
#include "emp/Evolve/Systematics.hpp"
#include "emp/Evolve/World_structure.hpp"
#include <limits>

#include "CPUState.h"
#include "SGPWorld.h"
#include <asmjit/x86.h>
#include <emp/base/vector.hpp>

std::mutex reproduce_mutex;
std::mutex rand_mutex;

// Helper functions called by the generated assembly for certain instructions
void do_reproduce(uint32_t *stack, CPUState *state) {
  // Only one reproduction is allowed per update
  if (state->in_progress_repro != -1)
    return;
  double points = state->host->IsHost()
                      ? state->world->GetConfig()->HOST_REPRO_RES()
                      : state->world->GetConfig()->SYM_HORIZ_TRANS_RES();
  if (state->host->GetPoints() > points) {
    state->host->AddPoints(-points);
    // Add this organism to the queue to reproduce, using the mutex to avoid a
    // data race
    std::lock_guard<std::mutex> lock(reproduce_mutex);
    state->in_progress_repro = state->world->to_reproduce.size();
    state->world->to_reproduce.push_back(
        std::pair(state->host, state->location));
  }
}
void do_private_io(uint32_t *stack, CPUState *state) {
  float score = state->world->GetTaskSet().CheckTasks(*state, *stack, false);
  if (score != 0.0) {
    if (!state->host->IsHost()) {
      state->world->GetSymEarnedDataNode().WithMonitor(
          [=](auto &m) { m.AddDatum(score); });
    } else {
      // A host loses 25% of points when performing private IO operations
      score *= 0.75;
    }
    state->host->AddPoints(score);
  }
  std::lock_guard<std::mutex> lock(rand_mutex);
  uint32_t next = state->world->GetRandom().GetBits50();
  *stack = next;
  state->input_buf.push(next);
}
void do_shared_io(uint32_t *stack, CPUState *state) {
  float score = state->world->GetTaskSet().CheckTasks(*state, *stack, true);
  if (score != 0.0) {
    if (!state->host->IsHost()) {
      state->world->GetSymEarnedDataNode().WithMonitor(
          [=](auto &m) { m.AddDatum(score); });
    }
    state->host->AddPoints(score);
  }
  std::lock_guard<std::mutex> lock(rand_mutex);
  uint32_t next = state->world->GetRandom().GetBits50();
  *stack = next;
  state->input_buf.push(next);
}
void do_donate(uint32_t *stack, CPUState *state) {
  if (state->host->IsHost())
    return;
  if (emp::Ptr<Organism> host = state->host->GetHost()) {
    // Donate 20% of the total points of the symbiont-host system
    // This way, a sym can donate e.g. 40 or 60 percent of their points in a
    // couple of instructions
    double to_donate =
        fmin(state->host->GetPoints(),
             (state->host->GetPoints() + host->GetPoints()) * 0.20);
    state->world->GetSymDonatedDataNode().WithMonitor(
        [=](auto &m) { m.AddDatum(to_donate); });
    host->AddPoints(to_donate);
    state->host->AddPoints(-to_donate);
  }
}

struct Instruction;

/*
REGISTER USE:
r8-r15: VM registers, r0-r7 in the genome code
rdi:    Stack pointer for the VM stack, grows upwards
rsi:    Pointer to the CPUState
rbx:    Counter for instructions executed this step
rbp:    The end of the stack space, leaving 36 (4*9) bytes above for saving
registers

r12-r15, rbx, rbp are callee-save, rest are caller-save
*/

class Assembler : public asmjit::x86::Assembler {
  std::unordered_map<uint8_t, asmjit::Label> labels;
  std::unordered_set<uint8_t> bound_labels;

public:
  Assembler(asmjit::CodeHolder *code, emp::vector<Instruction> *instructions);

  asmjit::Label matchLabel(uint8_t search) {
    if (labels.empty())
      return newLabel();
    if (labels.count(search))
      return labels.at(search);
    // Find closest
    return std::min_element(labels.begin(), labels.end(),
                            [&](auto x, auto y) {
                              return abs(x.first - search) <
                                     abs(y.first - search);
                            })
        ->second;
  }

  void bindLabel(uint8_t search) {
    if (bound_labels.count(search))
      return;
    bound_labels.insert(search);
    asmjit::Label label = labels.at(search);
    bind(label);
  }

  asmjit::x86::Gpd reg(uint8_t i) {
    // VM registers are r8d-r15d
    i = i % 8;
    return asmjit::x86::gpd(8 + i);
  }

  void makeCall(void (*callee)(uint32_t *, CPUState *)) {
    // Save caller-saved registers that we use - r8-r11, rdi, rsi
    for (int i = 8; i <= 11; i++) {
      push(asmjit::x86::gpq(i));
    }
    push(asmjit::x86::rdi);
    push(asmjit::x86::rsi);

    call(callee);

    pop(asmjit::x86::rsi);
    pop(asmjit::x86::rdi);
    for (int i = 11; i >= 8; i--) {
      pop(asmjit::x86::gpq(i));
    }
  }

  void save() {
    // Push all registers on top of the stack
    for (int i = 0; i < 8; i++) {
      mov(asmjit::x86::ptr(asmjit::x86::rdi), reg(i));
      add(asmjit::x86::rdi, 4); // 4 bytes
    }
    // Maintain 16-byte stack alignment
    add(asmjit::x86::rsp, 8);
    pop(asmjit::x86::rbx);
    pop(asmjit::x86::rbp);
    // Now the top of the stack is in rdi
    // Now load callee-save registers that we used - r12-r15
    // (in reverse order of push calls):
    for (int i = 15; i >= 12; i--) {
      pop(asmjit::x86::gpd(i));
    }
  }

  void load() {
    // First save callee-save registers that we use - r12-r15:
    for (int i = 12; i < 16; i++) {
      push(asmjit::x86::gpd(i));
    }
    push(asmjit::x86::rbp);
    push(asmjit::x86::rbx);
    // Maintain 16-byte stack alignment
    sub(asmjit::x86::rsp, 8);
    // Pop all registers off the stack
    for (int i = 7; i >= 0; i--) {
      sub(asmjit::x86::rdi, 4); // 4 bytes
      mov(reg(i), asmjit::x86::ptr(asmjit::x86::rdi));
    }
    // Now the top of the stack is restored
  }
};

enum Operation : uint8_t {
  Nop = 0,
  // single argument math
  ShiftLeft,
  ShiftRight,
  Increment,
  Decrement,
  // biological operations
  Reproduce,
  PrivateIO,
  SharedIO,
  // double argument math
  Add,
  Subtract,
  Nand,
  // Stack manipulation
  Push,
  Pop,
  SwapStack,
  Swap,
  Donate,
  JumpIfNEq,
  JumpIfLess,
  Label,
  Last,
};
struct Instruction {
  Operation op;
  uint8_t args[3];

  void assemble(Assembler &a) {
    switch (op % Last) {
    case Nop:
      break;
    case ShiftLeft:
      a.shl(a.reg(args[0]), 1);
      break;
    case ShiftRight:
      a.shr(a.reg(args[0]), 1);
      break;
    case Increment:
      a.inc(a.reg(args[0]));
      break;
    case Decrement:
      a.dec(a.reg(args[0]));
      break;
    case Reproduce:
      a.makeCall(do_reproduce);
      break;
    case PrivateIO:
      a.mov(asmjit::x86::ptr(asmjit::x86::rdi), a.reg(args[0]));
      a.makeCall(do_private_io);
      a.mov(a.reg(args[0]), asmjit::x86::ptr(asmjit::x86::rdi));
      break;
    case SharedIO:
      a.mov(asmjit::x86::ptr(asmjit::x86::rdi), a.reg(args[0]));
      a.makeCall(do_shared_io);
      a.mov(a.reg(args[0]), asmjit::x86::ptr(asmjit::x86::rdi));
      break;
    case Subtract:
      // a = b - c
      if (args[0] == args[1]) {
        a.sub(a.reg(args[0]), a.reg(args[2]));
        break;
      }
      a.mov(a.reg(args[0]), a.reg(args[2]));
      a.sub(a.reg(args[0]), a.reg(args[1]));
      break;
    case Add:
      // a = b + c
      if (args[0] == args[1]) {
        a.add(a.reg(args[0]), a.reg(args[2]));
        break;
      }
      a.mov(a.reg(args[0]), a.reg(args[2]));
      a.add(a.reg(args[0]), a.reg(args[1]));
      break;
    case Nand:
      // a = ^ (a & c)
      if (args[0] == args[1]) {
        a.and_(a.reg(args[0]), a.reg(args[2]));
        a.not_(a.reg(args[0]));
        break;
      }
      a.mov(a.reg(args[0]), a.reg(args[2]));
      a.and_(a.reg(args[0]), a.reg(args[1]));
      a.not_(a.reg(args[0]));
      break;
    // Stack pointer is in rdi
    case Push: {
      asmjit::Label skip = a.newLabel();
      a.cmp(asmjit::x86::rdi, asmjit::x86::rbp);
      a.jae(skip);
      a.mov(asmjit::x86::ptr(asmjit::x86::rdi), a.reg(args[0]));
      a.add(asmjit::x86::rdi, 4); // 4 bytes
      a.bind(skip);
      break;
    }
    case Pop: {
      asmjit::Label skip = a.newLabel();
      // If the stack pointer is more than 16*4 bytes from the end, don't pop
      // further. This way we don't need another register for the bottom of the
      // stack
      a.lea(asmjit::x86::rax, asmjit::x86::ptr(asmjit::x86::rdi, 16 * 4));
      a.cmp(asmjit::x86::rax, asmjit::x86::rbp);
      a.mov(a.reg(args[0]), 0);
      a.jbe(skip);
      a.sub(asmjit::x86::rdi, 4); // 4 bytes
      a.mov(a.reg(args[0]), asmjit::x86::ptr(asmjit::x86::rdi));
      a.bind(skip);
      break;
    }
    case SwapStack:
      // TODO multiple stacks
      break;
    case Swap:
      // use stack memory as a temporary; this is inefficient but who cares
      a.mov(asmjit::x86::ptr(asmjit::x86::rdi), a.reg(args[0]));
      a.mov(a.reg(args[0]), a.reg(args[1]));
      a.mov(a.reg(args[1]), asmjit::x86::ptr(asmjit::x86::rdi));
      break;
    case Donate:
      a.makeCall(do_donate);
      break;
    case JumpIfNEq:
      a.cmp(a.reg(args[0]), a.reg(args[1]));
      a.jne(a.matchLabel(args[2]));
      break;
    case JumpIfLess:
      a.cmp(a.reg(args[0]), a.reg(args[1]));
      a.jb(a.matchLabel(args[2]));
      break;
    case Label:
      a.bindLabel(args[0]);
      break;
    }
    // Check if we've run out of cycles yet
    a.dec(asmjit::x86::rbx);
    asmjit::Label next = a.newLabel();
    a.lea(asmjit::x86::rax, asmjit::x86::ptr(next));
    a.test(asmjit::x86::rbx, asmjit::x86::rbx);
    a.jz(a.labelByName("exit"));
    a.bind(next);
  }
};

Assembler::Assembler(asmjit::CodeHolder *code,
                     emp::vector<Instruction> *instructions)
    : asmjit::x86::Assembler(code) {
  for (Instruction &i : *instructions) {
    if (i.op % Last == Operation::Label) {
      labels.insert({i.args[0], newLabel()});
    }
  }
}

// A simple error handler implementation, extend according to your needs.
class MyErrorHandler : public asmjit::ErrorHandler {
public:
  void handleError(asmjit::Error err, const char *message,
                   asmjit::BaseEmitter *origin) override {
    printf("AsmJit error: %s\n", message);
  }
};

class Genome : public emp::vector<Instruction> {
  using jit_fun_t = uint32_t *(*)(uint32_t *stack, CPUState *state,
                                  uint32_t *stack_end, uint64_t cycles);

  asmjit::JitRuntime *rt;
  jit_fun_t cached_fun = nullptr;

public:
  Genome(asmjit::JitRuntime *rt) : rt(rt) {}

  Genome(const Genome &other) : emp::vector<Instruction>(other), rt(other.rt) {}

  ~Genome() {
    if (cached_fun != nullptr)
      rt->release(cached_fun);
  }

  void compile() {
    asmjit::CodeHolder code;
    MyErrorHandler handler;
    code.init(rt->environment());
    code.setErrorHandler(&handler);
    Assembler a(&code, this);

    asmjit::Label exit = a.newNamedLabel("exit");
    asmjit::Label start = a.newLabel();

    a.mov(asmjit::x86::rax, asmjit::x86::ptr(asmjit::x86::rdi));
    a.load();
    a.mov(asmjit::x86::rbx, asmjit::x86::rcx);
    a.mov(asmjit::x86::rbp, asmjit::x86::rdx);
    a.test(asmjit::x86::rax, asmjit::x86::rax);
    a.jz(start);
    a.jmp(asmjit::x86::rax);

    a.bind(start);
    for (Instruction &i : *this) {
      i.assemble(a);
    }
    // loop back to start
    a.jmp(start);

    a.bind(exit);
    a.save();
    // put rip on top
    a.mov(asmjit::x86::ptr(asmjit::x86::rdi), asmjit::x86::rax);
    // Return the new stack pointer
    a.mov(asmjit::x86::rax, asmjit::x86::rdi);
    a.ret();

    auto err = rt->add(&cached_fun, &code);
    if (err) {
      std::cerr << err << std::endl;
      std::terminate();
    }
  }

  void run(uint32_t **stack, CPUState *state, uint32_t *stack_end,
           uint64_t cycles) {
    if (cached_fun == nullptr) {
      std::cout << "run() without cached fun!" << std::endl;
      std::terminate();
    }
    *stack = cached_fun(*stack, state, stack_end, cycles);
  }

  void Mutate(emp::Random &random) {
    uint32_t *ptr = reinterpret_cast<uint32_t *>(this->data());
    for (size_t i = 0; i < size(); i++) {
      // 6.25% chance to flip each bit, because smaller would be hard with
      // emp::Random
      ptr[i] ^= random.GetBits12_5() & random.GetBits12_5();
    }
  }

  void Print() {
    emp::vector<uint8_t> labels;
    for (Instruction &i : *this) {
      if (i.op % Last == Operation::Label) {
        labels.push_back(i.args[0]);
      }
    }
    std::vector<std::string> op_names{
        "Nop",
        // single argument math
        "ShiftLeft", "ShiftRight", "Increment", "Decrement",
        // biological operations
        "Reproduce", "PrivateIO", "SharedIO",
        // double argument math
        "Add", "Subtract", "Nand",
        // Stack manipulation
        "Push", "Pop", "SwapStack", "Swap", "Donate", "JumpIfNEq", "JumpIfLess",
        "Label"};
    std::unordered_map<std::string, uint8_t> op_arities{{"Nop", 0},
                                                        // single argument math
                                                        {"ShiftLeft", 1},
                                                        {"ShiftRight", 1},
                                                        {"Increment", 1},
                                                        {"Decrement", 1},
                                                        // biological operations
                                                        {"Reproduce", 0},
                                                        {"PrivateIO", 1},
                                                        {"SharedIO", 1},
                                                        // double argument math
                                                        {"Add", 3},
                                                        {"Subtract", 3},
                                                        {"Nand", 3},
                                                        // Stack manipulation
                                                        {"Push", 1},
                                                        {"Pop", 1},
                                                        {"SwapStack", 0},
                                                        {"Swap", 2},
                                                        {"Donate", 0},
                                                        {"JumpIfNEq", 2},
                                                        {"JumpIfLess", 2},
                                                        {"Label", 0}};
    for (Instruction &inst : *this) {
      Operation op = (Operation)(inst.op % Operation::Last);
      if (op == Operation::Label) {
        std::cout << 'L' << (int)inst.args[0] << ":\n";
        continue;
      }
      std::cout << "    " << op_names[op];
      for (int i = op_names[op].size(); i < 12; i++) {
        std::cout << ' ';
      }
      for (int i = 0; i < op_arities[op_names[op]]; i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << 'r' << (int)(inst.args[i] % 8);
      }
      if (op == Operation::JumpIfLess || op == Operation::JumpIfNEq) {
        uint8_t search = inst.args[2];
        auto found = std::min_element(
            labels.begin(), labels.end(), [&](uint8_t x, uint8_t y) {
              return abs(x - search) < abs(y - search);
            });
        if (found == labels.end())
          std::cout << ", <nowhere (" << (int)search << ")>";
        else
          std::cout << ", L" << (int)*found;
      }
      std::cout << '\n';
    }
    std::cout << std::endl;
  }
};

#endif