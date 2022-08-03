#ifndef GENOME_LIBRARY
#define GENOME_LIBRARY

#include "JIT.h"
#include "asmjit/core/jitruntime.h"
#include <cstddef>
#include <limits>

const size_t PROGRAM_LENGTH = 100;

/**
 * Allows building up a program without knowing the final size.
 * When it's done and `build()` is called, the instructions added to the builder
 * will be located at the end of the generated program, right before
 * `reproduce`.
 */
class ProgramBuilder : emp::vector<Instruction> {
public:
  void add(Operation op, uint8_t arg0 = 0, uint8_t arg1 = 0, uint8_t arg2 = 0) {
    push_back(Instruction{op, {arg0, arg1, arg2}});
  }

  Genome build(asmjit::JitRuntime *rt, size_t length) {
    add(Reproduce);

    Genome program(rt);
    // Set everything to 0 - this makes them no-ops since that's the first
    // inst in the library
    program.resize(length - size());

    program.insert(program.end(), begin(), end());

    return program;
  }

  void addNot() {
    // sharedio   r0
    // nand       r0, r0, r0
    // sharedio   r0
    add(SharedIO);
    add(Nand);
    add(SharedIO);
  }

  void addSquare() {
    // Always output 4:
    // pop        r0
    // increment  r0          -> 1
    // add        r0, r0, r0  -> 2
    // add        r0, r0, r0  -> 4
    // sharedio   r0
    add(Pop);
    add(Increment);
    add(Add);
    add(Add);
    add(SharedIO);
    add(Reproduce);
  }

  void addNand() {
    // sharedio   r0
    // sharedio   r1
    // nand       r0, r1, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand, 0, 1, 0);
    add(SharedIO);
  }

  void addAnd() {
    // ~(a nand b)
    // sharedio   r0
    // sharedio   r1
    // nand       r0, r1, r0
    // nand       r0, r0, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand, 0, 1, 0);
    add(Nand);
    add(SharedIO);
  }

  void addOrn() {
    // (~a) nand b
    // sharedio   r0
    // sharedio   r1
    // nand       r0, r0, r0
    // nand       r0, r1, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand);
    add(Nand, 0, 1, 0);
    add(SharedIO);
  }

  void addOr() {
    // (~a) nand (~b)
    // sharedio   r0
    // sharedio   r1
    // nand       r0, r0, r0
    // nand       r1, r1, r1
    // nand       r0, r1, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand, 0, 0, 0);
    add(Nand, 1, 1, 1);
    add(Nand, 0, 1, 0);
    add(SharedIO);
  }

  void addAndn() {
    // ~(a nand (~b))
    // sharedio   r0
    // sharedio   r1
    // nand       r1, r1, r1
    // nand       r0, r1, r0
    // nand       r0, r0, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand, 1, 1, 1);
    add(Nand, 0, 1, 0);
    add(Nand, 0, 0, 0);
    add(SharedIO);
  }

  void addNor() {
    // ~((~a) nand (~b))
    // sharedio   r0
    // sharedio   r1
    // nand       r0, r0, r0
    // nand       r1, r1, r1
    // nand       r0, r1, r0
    // nand       r0, r0, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);
    add(Nand, 0, 0, 0);
    add(Nand, 1, 1, 1);
    add(Nand, 0, 1, 0);
    add(Nand, 0, 0, 0);
    add(SharedIO);
  }

  void addXor() {
    // (a & ~b) | (~a & b) --> (a nand ~b) nand (~a nand b)
    // sharedio   r0
    // sharedio   r1
    //
    // nand       r3, r1, r1
    // nand       r3, r3, r0
    //
    // nand       r2, r0, r0
    // nand       r2, r2, r1
    //
    // nand       r0, r2, r3
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);

    add(Nand, 3, 1, 1);
    add(Nand, 3, 3, 0);

    add(Nand, 2, 0, 0);
    add(Nand, 2, 2, 1);

    add(Nand, 0, 2, 3);
    add(SharedIO);
  }

  void addEqu() {
    // ~(a ^ b)
    // sharedio   r0
    // sharedio   r1
    //
    // nand       r3, r1, r1
    // nand       r3, r3, r0
    //
    // nand       r2, r0, r0
    // nand       r2, r2, r1
    //
    // nand       r0, r2, r3
    // nand       r0, r0, r0
    // sharedio   r0
    add(SharedIO);
    add(SharedIO, 1);

    add(Nand, 3, 1, 1);
    add(Nand, 3, 3, 0);

    add(Nand, 2, 0, 0);
    add(Nand, 2, 2, 1);

    add(Nand, 0, 2, 3);
    add(Nand, 0, 0, 0);
    add(SharedIO);
  }
};

Genome CreateRandomProgram(asmjit::JitRuntime *rt, emp::Random &random,
                           size_t length) {
  Genome genome(rt);
  genome.resize(100);
  random.RandFill(reinterpret_cast<unsigned char *>(genome.data()),
                  genome.size() * sizeof(Instruction));
  return genome;
}

Genome CreateNotProgram(asmjit::JitRuntime *rt, size_t length) {
  ProgramBuilder program;
  program.addNot();
  return program.build(rt, length);
}

Genome CreateSquareProgram(asmjit::JitRuntime *rt, size_t length) {
  ProgramBuilder program;
  program.addSquare();
  return program.build(rt, length);
}

/**
 * Picks what type of starting program should be created based on the config and
 * creates it. It will be either random, a program that does NOT, or a program
 * that does SQUARE (which always outputs 4).
 */
Genome CreateStartProgram(asmjit::JitRuntime *rt, emp::Random &random, emp::Ptr<SymConfigBase> config) {
  if (config->RANDOM_ANCESTOR()) {
    return CreateRandomProgram(rt, random, PROGRAM_LENGTH);
  } else if (config->TASK_TYPE() == 1) {
    return CreateNotProgram(rt, PROGRAM_LENGTH);
  } else {
    return CreateSquareProgram(rt, PROGRAM_LENGTH);
  }
}

#endif