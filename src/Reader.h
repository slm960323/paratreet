#ifndef PARATREET_READER_H_
#define PARATREET_READER_H_

#include "paratreet.decl.h"
#include "common.h"
#include "ParticleMsg.h"
#include "BoundingBox.h"
#include "Splitter.h"
#include "Utility.h"
#include "TreeSpec.h"
#include "Modularization.h"

extern int n_readers;
extern CProxy_TreeSpec treespec;

class Reader : public CBase_Reader {
  BoundingBox box;
  std::vector<Particle> particles;
  std::vector<ParticleMsg*> particle_messages;
  int particle_index;

  public:
    BoundingBox universe;
    // std::vector<Splitter> splitters;
    // std::vector<Key> SFCsplitters;
    Reader();

    // Loading particles and assigning keys
    void load(std::string, const CkCallback&);
    void computeUniverseBoundingBox(const CkCallback& cb);
    void assignKeys(BoundingBox, const CkCallback&);

    // OCT decomposition
    void countOct(std::vector<Key>, size_t, const CkCallback&);

    // SFC decomposition
    //void countSfc(const std::vector<Key>&, const CkCallback&);
    void pickSamples(const int, const CkCallback&);
    void prepMessages(const std::vector<Key>&, const CkCallback&);
    void redistribute();
    void receive(ParticleMsg*);
    void localSort(const CkCallback&);
    void checkSort(const Key, const CkCallback&);
    template <typename Data>
    void request(CProxy_TreePiece<Data>, int, int);

    // Sending particles to home TreePieces
    template <typename Data>
    void flush(int, int, CProxy_TreePiece<Data>);
};

template <typename Data>
void Reader::request(CProxy_TreePiece<Data> tp_proxy, int index, int num_to_give) {
  int n_particles = box.n_particles;
  ParticleMsg* msg = new (num_to_give) ParticleMsg(&particles[0], num_to_give);
  for (int i = 0; i < n_particles - num_to_give; i++) {
    particles[i] = particles[num_to_give + i];
  } // maybe a function can just do this for me?
  particles.resize(n_particles - num_to_give);
  n_particles -= num_to_give;
  // if (n_particles) SFCsplitters.push_back(particles[0].key);
  tp_proxy[index].receive(msg);
}

template <typename Data>
void Reader::flush(int n_total_particles, int n_treepieces, CProxy_TreePiece<Data> treepieces) {
  auto sendParticles = [&](int dest, int n_particles, Particle* particles) {
    ParticleMsg* msg = new (n_particles) ParticleMsg(particles, n_particles);
    treepieces[dest].receive(msg);
  };

  int flush_count =
      treespec.ckLocalBranch()->getDecomposition()->flush(n_total_particles, n_treepieces, sendParticles, particles);
  if (flush_count != particles.size()) {
    CkPrintf("Reader %d failure: flushed %d out of %zu particles\n", thisIndex,
        flush_count, particles.size());
    CkAbort("Reader failure -- see stdout");
  }

  // Clean up
  particles.clear();
  particle_index = 0;
}

#endif // PARATREET_READER_H_
