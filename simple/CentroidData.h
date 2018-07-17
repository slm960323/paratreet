#ifndef SIMPLE_CENTROIDDATA_H_
#define SIMPLE_CENTROIDDATA_H_

#include "common.h"
#include <vector>
#include <queue>
#include "Particle.h"
#include "ParticleComp.h"
#include "OrientedBox.h"


struct CentroidData {
  Vector3D<Real> moment;
  Real sum_mass;
  std::vector<Vector3D<Real> > sum_forces;
  std::vector< std::priority_queue<Particle, std::vector<Particle>, particle_comp> > neighbors;
  std::vector< std::vector<Particle> > nearby;
  OrientedBox<Real> box;
  int count;

  CentroidData() : 
  moment(Vector3D<Real> (0,0,0)), sum_mass(0),
  sum_forces(std::vector< Vector3D<Real> > ()), count(0) {}

  const CentroidData& operator+ (const CentroidData& cd) { // needed for upward traversal
    moment += cd.moment;
    sum_mass += cd.sum_mass;
    box.grow(cd.box);
    count += cd.count;
    return *this;
  }
  const CentroidData& operator= (const CentroidData& cd) {
    moment = cd.moment;
    sum_mass = cd.sum_mass;
    box = cd.box;
    count = cd.count;
    return *this;
  }
  void pup(PUP::er& p) {
    p | moment;
    p | sum_mass;
    p | box;
    p | count;
  }
  Vector3D<Real> getCentroid() {
    return moment / sum_mass;
  }
};

#endif // SIMPLE_CENTROID_H_
