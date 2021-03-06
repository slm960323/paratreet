#ifndef PARATREET_TREESPEC_H_
#define PARATREET_TREESPEC_H_

#include "paratreet.decl.h"
#include "Node.h"
#include "Modularization.h"

class TreeSpec : public CBase_TreeSpec {
public:
    TreeSpec(const paratreet::Configuration& config_)
    : config(config_),
      decomp(nullptr),
      tree(nullptr) { }

    void check(const CkCallback &cb);
    void receiveDecomposition(CkMarshallMsg*);
    Decomposition* getDecomposition();

    Tree* getTree();

    int doFindSplitters(BoundingBox &universe, CProxy_Reader &readers) {
        int log_branch_factor = log2(getTree()->getBranchFactor());
        return getDecomposition()->findSplitters(universe, readers, log_branch_factor);
    }

    void receiveConfiguration(const paratreet::Configuration&,CkCallback);
    paratreet::Configuration& getConfiguration();
    void setConfiguration(const paratreet::Configuration& config_) { config = config_; }

    template <typename Data>
    Node<Data>* makeNode(Key key, int depth, int n_particles, Particle* particles, int owner_tp_start, int owner_tp_end, bool is_leaf, Node<Data>* parent, int tp_index) {
      switch (getTree()->getBranchFactor()) {
      case 2:
        return new FullNode<Data, 2> (key, depth, n_particles, particles, owner_tp_start, owner_tp_end, is_leaf, parent, tp_index);
      case 8:
        return new FullNode<Data, 8> (key, depth, n_particles, particles, owner_tp_start, owner_tp_end, is_leaf, parent, tp_index);
      default:
        return nullptr;
      }
    }

    template <typename Data>
    Node<Data>* makeCachedNode(Key key, typename Node<Data>::Type type, SpatialNode<Data> spatial_node, Node<Data>* parent, const Particle* particlesToCopy) {
      Particle* particles = nullptr;
      if (spatial_node.is_leaf && spatial_node.n_particles > 0) {
        particles = new Particle [spatial_node.n_particles];
        std::copy(particlesToCopy, particlesToCopy + spatial_node.n_particles, particles);
      }
      switch (getTree()->getBranchFactor()) {
      case 2:
        return new FullNode<Data, 2> (key, type, spatial_node.is_leaf, spatial_node, particles, parent);
      case 8:
        return new FullNode<Data, 8> (key, type, spatial_node.is_leaf, spatial_node, particles, parent);
      default:
        return nullptr;
      }
    }

protected:
    std::unique_ptr<Tree> tree;
    std::unique_ptr<Decomposition> decomp;
    paratreet::Configuration config;
};

#endif
