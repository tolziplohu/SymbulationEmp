#ifndef SGPCPU_H
#define SGPCPU_H

#include "../default_mode/Host.h"
#include "CPUState.h"
#include "GenomeLibrary.h"
#include "JIT.h"
#include "SGPWorld.h"
#include "Tasks.h"
#include <cmath>
#include <iostream>
#include <ostream>
#include <string>

/**
 * Represents the virtual CPU and the program genome for an organism in the SGP
 * mode.
 */
class CPU {
  emp::Ptr<emp::Random> random;
  uint32_t *stack_buffer = new uint32_t[64];
  uint32_t *stack_pointer = &stack_buffer[32];
  uint32_t *stack_end = &stack_buffer[40];

  void InitializeState() {
    for (int i = 0; i < 64; i++) {
      stack_buffer[i] = 0;
    }
    this->genome.compile();
    state.self_completed.resize(state.world->GetTaskSet().NumTasks());
    state.shared_completed->resize(state.world->GetTaskSet().NumTasks());
  }

public:
  Genome genome;
  CPUState state;

  /**
   * Constructs a new CPU for an ancestor organism, with either a random genome
   * or a blank genome that knows how to do a simple task depending on the
   * config setting RANDOM_ANCESTOR.
   */
  CPU(emp::Ptr<Organism> organism, emp::Ptr<SGPWorld> world,
      emp::Ptr<emp::Random> random)
      : random(random),
        genome(CreateStartProgram(&world->rt, *random, world->GetConfig())),
        state(organism, world) {
    InitializeState();
  }

  /**
   * Constructs a new CPU with a copy of another CPU's genome.
   */
  CPU(emp::Ptr<Organism> organism, emp::Ptr<SGPWorld> world,
      emp::Ptr<emp::Random> random, const Genome &genome)
      : random(random), genome(genome), state(organism, world) {
    InitializeState();
  }

  CPU(const CPU &other)
      : random(other.random), genome(other.genome),
        state(other.state.host, other.state.world) {
    InitializeState();
  }

  /**
   * Input: None
   *
   * Output: None
   *
   * Purpose: Resets the CPU to its initial state.
   */
  void Reset() {
    for (int i = 0; i < 64; i++) {
      stack_buffer[i] = 0;
    }
    stack_pointer = &stack_buffer[32];
    state = CPUState(state.host, state.world);
    state.self_completed.resize(state.world->GetTaskSet().NumTasks());
    state.shared_completed->resize(state.world->GetTaskSet().NumTasks());
  }

  /**
   * Input: The location of the organism (used for reproduction), and the number
   * of CPU cycles to run. If the organism shouldn't be allowed to reproduce,
   * then the location should be `emp::WorldPosition::invalid_id`.
   *
   * Output: None
   *
   * Purpose: Steps the CPU forward a certain number of cycles.
   */
  void RunCPUStep(emp::WorldPosition location, size_t n_cycles) {
    state.location = location;

    genome.run(&stack_pointer, &state, stack_end, n_cycles);
  }

  /**
   * Input: None
   *
   * Output: None
   *
   * Purpose: Mutates the genome code stored in the CPU.
   */
  void Mutate() {
    genome.Mutate(*random);
    genome.compile();
  }

public:
  /**
   * Input: None
   *
   * Output: None
   *
   * Purpose: Prints out a human-readable representation of the program code of
   * the organism's genome to standard output.
   */
  void PrintCode(std::ostream &out = std::cout) { genome.Print(out); }
};

#endif
