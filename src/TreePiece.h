#ifndef PARATREET_TREEPIECE_H_
#define PARATREET_TREEPIECE_H_

#include "paratreet.decl.h"
#include "common.h"
#include "templates.h"
#include "ParticleMsg.h"
#include "Node.h"
#include "Utility.h"
#include "Reader.h"
#include "CacheManager.h"
#include "Resumer.h"
#include "Traverser.h"
#include "Driver.h"
#include "OrientedBox.h"

#include <cstring>
#include <queue>
#include <vector>
#include <fstream>

extern CProxy_TreeSpec treespec;
extern CProxy_Reader readers;
extern CProxy_Driver<CentroidData> centroid_driver;

template <typename Data>
class TreePiece : public CBase_TreePiece<Data> {
public:
  std::vector<Particle> particles, incoming_particles;
  std::vector<Node<Data>*> leaves;
  std::vector<Node<Data>*> empty_leaves;

  int n_total_particles;
  int n_treepieces;
  int particle_index;
  int n_expected;

  Key tp_key; // Should be a prefix of all particle keys underneath this node
  Node<Data>* global_root; // Root of the global tree structure
  Node<Data>* local_root; // Root node of this TreePiece, TreeCanopies sit above this node

  Traverser<Data>* traverser;

  CProxy_TreeCanopy<Data> tc_proxy;
  CProxy_CacheManager<Data> cm_proxy;
  CacheManager<Data>* cm_local;
  CProxy_Resumer<Data> r_proxy;
  Resumer<Data>* r_local;

  std::vector<std::vector<Node<Data>*>> interactions;
  bool cache_init;
  std::vector<Particle> flushed_particles; // For debugging
  int dim_cnt;

  TreePiece(const CkCallback&, int, int, TCHolder<Data>,
    CProxy_Resumer<Data>, CProxy_CacheManager<Data>, DPHolder<Data>);
  void receive(ParticleMsg*);
  void check(const CkCallback&);
  void triggerRequest();
  void buildTree();
  bool recursiveBuild(Node<Data>*, Particle* node_particles, bool, size_t);
  void populateTree();
  inline void initCache();
  void requestNodes(Key, int);
  template<typename Visitor> void startDown();
  template<typename Visitor> void startUpAndDown();
  template<typename Visitor> void startDual();
  void goDown(Key);
  void interact(const CkCallback&);
  void print(Node<Data>*);
  void calculateMigrateRatio(Real timestep);
  void perturb (Real timestep, bool);
  void flush(CProxy_Reader);
  void destroy();
  void output(CProxy_Writer w, CkCallback cb);

  // For debugging
  void checkParticlesChanged(const CkCallback& cb) {
    bool result = true;
    if (particles.size() != flushed_particles.size()) {
      result = false;
      this->contribute(sizeof(bool), &result, CkReduction::logical_and_bool, cb);
      return;
    }
    for (int i = 0; i < particles.size(); i++) {
      if (!(particles[i] == flushed_particles[i])) {
        result = false;
        break;
      }
    }
    this->contribute(sizeof(bool), &result, CkReduction::logical_and_bool, cb);
  }
};

template <typename Data>
TreePiece<Data>::TreePiece(const CkCallback& cb, int n_total_particles_,
    int n_treepieces_, TCHolder<Data> tc_holder, CProxy_Resumer<Data> r_proxy_,
    CProxy_CacheManager<Data> cm_proxy_, DPHolder<Data> dp_holder) {
  n_total_particles = n_total_particles_;
  n_treepieces = n_treepieces_;
  particle_index = 0;

  tc_proxy = tc_holder.proxy;
  r_proxy = r_proxy_;
  r_local = r_proxy.ckLocalBranch();
  r_local->tp_proxy = this->thisProxy;
  r_local->resume_nodes_per_tp.resize(n_treepieces);
  cm_proxy = cm_proxy_;
  cm_local = cm_proxy.ckLocalBranch();
  r_local->cm_local = cm_local;
  cm_local->r_proxy = r_proxy;

  cache_init = false;
  dim_cnt = 0;

  n_expected = treespec.ckLocalBranch()->getDecomposition()->
      getNumExpectedParticles(n_total_particles, n_treepieces, this->thisIndex);

  tp_key = treespec.ckLocalBranch()->getDecomposition()->
      getTpKey(this->thisIndex);

  // Create TreeCanopies and send proxies
  auto sendProxy = [&](int dest, int tp_index) {
    tc_proxy[dest].recvProxies(TPHolder<Data>(this->thisProxy), tp_index, cm_proxy, dp_holder);
  };

  treespec.ckLocalBranch()->getTree()->buildCanopy(this->thisIndex, sendProxy);

  global_root = nullptr;
  local_root = nullptr;

  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::receive(ParticleMsg* msg) {
  // Copy particles to local vector
  // TODO: Remove memcpy by just storing the pointer to msg->particles
  // and using it in tree build
  int initial_size = incoming_particles.size();
  incoming_particles.resize(initial_size + msg->n_particles);
  std::memcpy(&incoming_particles[initial_size], msg->particles, msg->n_particles * sizeof(Particle));
  particle_index += msg->n_particles;
  delete msg;
}

template <typename Data>
void TreePiece<Data>::check(const CkCallback& cb) {
  if (n_expected != incoming_particles.size()) {
    CkPrintf("[TP %d] ERROR! Only %zu particles out of %d received",
        this->thisIndex, particles.size(), n_expected);
    CkAbort("TreePiece particle receipt failure -- see stdout");
  }

  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::triggerRequest() {
  readers.ckLocalBranch()->request(this->thisProxy, this->thisIndex, n_expected);
}

template <typename Data>
void TreePiece<Data>::buildTree() {
  int n_particles_saved = particles.size();
  int n_particles_received = incoming_particles.size();

  // Copy over received particles
  particles.resize(n_particles_saved + n_particles_received);
  std::copy(incoming_particles.begin(), incoming_particles.end(), particles.begin() + n_particles_saved);
  incoming_particles.clear();

  // Sort particles
  std::sort(particles.begin(), particles.end());

  // Clear existing data
  leaves.clear();
  empty_leaves.clear();

  // Create global root and build local tree recursively
#if DEBUG
  CkPrintf("[TP %d] key: 0x%" PRIx64 " particles: %d\n", this->thisIndex, tp_key, particles.size());
#endif
  global_root = treespec.ckLocalBranch()->template makeNode<Data>(1, 0, particles.size(), &particles[0], 0, n_treepieces - 1, false, nullptr, this->thisIndex);
  recursiveBuild(global_root, &particles[0], false, log2(global_root->getBranchFactor()));

  // Initialize interactions vector: filled in during traversal
  interactions = std::vector<std::vector<Node<Data>*>>(leaves.size());

  // Populate the tree structure (including TreeCanopy)
  populateTree();
  // Initialize cache
  initCache();
}

template <typename Data>
bool TreePiece<Data>::recursiveBuild(Node<Data>* node, Particle* node_particles, bool saw_tp_key, size_t log_branch_factor) {
#if DEBUG
  //CkPrintf("[Level %d] created node 0x%" PRIx64 " with %d particles\n",
    //  node->depth, node->key, node->n_particles);
#endif
  // store reference to splitters
  //static std::vector<Splitter>& splitters = readers.ckLocalBranch()->splitters;
  auto config = treespec.ckLocalBranch()->getConfiguration();

  // Check if we are inside the subtree rooted at the treepiece's key
  if (!saw_tp_key) {
    saw_tp_key = (node->key == tp_key);
    if (saw_tp_key) local_root = node;
  }

  bool is_light = (node->n_particles <= ceil(BUCKET_TOLERANCE * config.max_particles_per_leaf));
  bool is_prefix = Utility::isPrefix(node->key, tp_key, log_branch_factor);

  if (saw_tp_key) {
    // If we are under the TP key, we can stop going deeper if node is light
    if (is_light) {
      if (node->n_particles == 0) {
        node->type = Node<Data>::Type::EmptyLeaf;
        empty_leaves.push_back(node);
      }
      else {
        node->type = Node<Data>::Type::Leaf;
        leaves.push_back(node);
      }
      return true;
    }
  } else { // Still on the way to TP, or we could have diverged
    if (!is_prefix) {
      // Diverged, should be marked as are mote node
      node->type = Node<Data>::Type::Remote;

      return false;
    }
  }

  // For all other cases, we go deeper
  /* XXX: For SFC?
  int owner_start = node->owner_tp_start;
  int owner_end = node->owner_tp_end;
  bool single_owner = (owner_start == owner_end);

  if (is_light) {
    if (saw_tp_key) {
      // we can make the node a local leaf
      if (node->n_particles == 0)
        node->type = Node<Data>::EmptyLeaf;
      else
        node->type = Node<Data>::Leaf;

        return true;
    }
    else if (!is_prefix) {
      // we have diverged from the path to the subtree rooted at the treepiece's key
      // so designate as remote
      node->type = Node<Data>::Remote;

      CkAssert(node->n_particles == 0);
      CkAssert(node->n_children == 0);

      if (single_owner) {
        int assigned = splitters[owner_start].n_particles;
        if (assigned == 0) {
          node->type = Node<Data>::RemoteEmptyLeaf;
        }
        else if (assigned <= BUCKET_TOLERANCE * max_particles_per_leaf) {
          node->type = Node<Data>::RemoteLeaf;
        }
        else {
          node->type = Node<Data>::Remote;
          node->n_children = BRANCH_FACTOR;
        }
      }
      else {
        node->type = Node<Data>::Remote;
        node->n_children = BRANCH_FACTOR;
      }

      return false;
    }
    else {
      CkAbort("Error: can a light node be an internal node (above a TP root)?");
    }
  }
  */

  // Create children
  node->n_children = node->wait_count = (1 << log_branch_factor);
  node->is_leaf = false;
  Key child_key = (node->key << log_branch_factor);
  int start = 0;
  int finish = start + node->n_particles;
  int non_local_children = 0;

  for (int i = 0; i < node->n_children; i++) {
    int first_ge_idx = OctTree::findChildsLastParticle(node, i, child_key, start, finish, log_branch_factor);
    int n_particles = first_ge_idx - start;

    /*
    // find owner treepieces of child
    int child_owner_start = owner_start;
    int child_owner_end;
    if (single_owner) {
      child_owner_end = child_owner_start;
    }
    else {
      if (i < node->n_children - 1) {
        int first_ge_tp = Utility::binarySearchGE(sibling_splitter, &splitters[0], owner_start, owner_end);
        child_owner_end = first_ge_tp - 1;
        owner_start = first_ge_tp;
      }
      else {
        child_owner_end = owner_end;
      }
    }
    */

    // Create child and store in vector
    Node<Data>* child = treespec.ckLocalBranch()->template makeNode<Data>(child_key, node->depth + 1,
        n_particles, node_particles + start, 0, n_treepieces - 1, true, node, this->thisIndex);
    /*
    Node<Data>* child = new Node<Data>(child_key, node->depth + 1, node->particles + start,
        n_particles, child_owner_start, child_owner_end, node);
    */
    node->exchangeChild(i, child);;

    // Recursive tree build
    bool local = recursiveBuild(child, node_particles + start, saw_tp_key, log_branch_factor);
    if (!local) non_local_children++;

    start = first_ge_idx;
    child_key++;
  }

  if (non_local_children == 0) {
    // All children are local, this is an internal node
    node->type = Node<Data>::Type::Internal;
  }
  else {
    // Some or all children are remote, this is a boundary node
    node->type = Node<Data>::Type::Boundary;
    node->tp_index = -1;
  }

  return (non_local_children == 0);
}

template <typename Data>
void TreePiece<Data>::populateTree() {
  // Populates the global tree structure by going up the tree
  std::queue<Node<Data>*> going_up;

  for (auto leaf : leaves) {
    leaf->data = Data(leaf->particles(), leaf->n_particles);
    going_up.push(leaf);
  }

  if (!leaves.size()) going_up.push(local_root);
  else for (auto empty_leaf : empty_leaves) going_up.push(empty_leaf);

  while (going_up.size()) {
    Node<Data>* node = going_up.front();
    going_up.pop();
    if (node->key == tp_key) {
      // We are at the root of the TreePiece, send accumulated data to
      // parent TreeCanopy
      int branch_factor = node->getBranchFactor();
      size_t tc_key = tp_key / branch_factor;
      if (tc_key > 0) tc_proxy[tc_key].recvData(*node, branch_factor);
    } else {
      // Add this node's data to the parent, and add parent to the queue
      // if all children have contributed
      Node<Data>* parent = node->parent;
      parent->data += node->data;
      parent->wait_count--;
      if (parent->wait_count == 0) going_up.push(parent);
    }
  }
}

template <typename Data>
void TreePiece<Data>::initCache() {
  if (!cache_init) {
    cm_local->connect(local_root, false);
    auto local_parent = local_root->parent;
    if (local_parent) {
      local_parent->exchangeChild(tp_key % local_root->getBranchFactor(),nullptr);
      global_root->triggerFree();
    }
    cache_init = true;
  }
}

template <typename Data>
template <typename Visitor>
void TreePiece<Data>::startDown() {
  traverser = new DownTraverser<Data, Visitor>(*this);
  traverser->start();
}

template <typename Data>
template <typename Visitor>
void TreePiece<Data>::startUpAndDown() {
  traverser = new UpnDTraverser<Data, Visitor>(*this);
  traverser->start();
}

template <typename Data>
template <typename Visitor>
void TreePiece<Data>::startDual() {
  auto && keys = ((SfcDecomposition*)treespec.ckLocalBranch()->getDecomposition())->getAllTpKeys(n_treepieces);
  traverser = new DualTraverser<Data, Visitor>(*this, keys);
  traverser->start();
  // root needs to be the root of the searched tree, not the searching tree
}

template <typename Data>
void TreePiece<Data>::requestNodes(Key key, int cm_index) {
  Node<Data>* node = local_root->getDescendant(key);
  if (!node) CkPrintf("null found for key %lu on tp %d\n", key, this->thisIndex);
  cm_local->serviceRequest(node, cm_index);
}

template <typename Data>
void TreePiece<Data>::goDown(Key new_key) {
  traverser->resumeTrav(new_key);
}

template <typename Data>
void TreePiece<Data>::interact(const CkCallback& cb) {
  traverser->interact();
  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::calculateMigrateRatio (Real timestep) {
  int migrateCount = 0;
  int particlesSize = particles.size();
  Real maxVelocity;
  int tupleSize = 3;
  if (particles.empty()) {
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(int), &migrateCount, CkReduction::sum_int),
      CkReduction::tupleElement(sizeof(Real), &maxVelocity, CkReduction::max_double),
      CkReduction::tupleElement(sizeof(int), &particlesSize, CkReduction::max_int),
    };
    CkReductionMsg * msg = CkReductionMsg::buildFromTuple(tupleRedn, tupleSize);
    CkCallback cb (CkReductionTarget(Driver<CentroidData>, treepiecesReportMigrateCountAndMaxVelocity), centroid_driver);
    msg->setCallback(cb);
    this->contribute(msg);
    return;
  };

  std::vector<int> remainders;
  Key temp = tp_key;
  //CkPrintf("=== tp_idx = [%d] leaves[0] = %p\n", local_root->tp_index, leaves[0]);
  const size_t branch_factor = (leaves.size() > 0? leaves[0]->getBranchFactor() :  empty_leaves[0]->getBranchFactor());
  //CkPrintf("=== tp_idx = [%d] branch_factor = %d; tp_key = %d \n", local_root->tp_index, branch_factor, tp_key);
  while (temp >= branch_factor) {
    remainders.push_back(temp % branch_factor);
    temp /= branch_factor;
  }
  OrientedBox<Real> tp_box = readers.ckLocalBranch()->universe.box;
  for (int i = remainders.size()-1; i >= 0; i--) {
    for (int dim = 0; dim < 3; dim++) {
      if (remainders[i] & (1 << (2-dim))) tp_box.lesser_corner[dim] = tp_box.center()[dim];
      else tp_box.greater_corner[dim] = tp_box.center()[dim];
    }
  }

  for (auto& particle : particles){
    particle.perturb(timestep, readers.ckLocalBranch()->universe.box);
    if (!tp_box.contains(particle.position)) {
      migrateCount ++;
    }
    if (particle.velocity.length() > maxVelocity)
        maxVelocity = particle.velocity.length();
  }
  //CkPrintf("=== tp_idx = [%d] migrateCount = %d; ratio = %f; maxVelocity = %f \n", local_root->tp_index, migrateCount, (double)(migrateCount / particles.size()), maxVelocity);
  CkReduction::tupleElement tupleRedn[] = {
    CkReduction::tupleElement(sizeof(unsigned long long), &migrateCount, CkReduction::sum_ulong_long),
    CkReduction::tupleElement(sizeof(Real), &maxVelocity, CkReduction::max_float),
    CkReduction::tupleElement(sizeof(int), &particlesSize, CkReduction::max_int),
  };

  CkReductionMsg * msg = CkReductionMsg::buildFromTuple(tupleRedn, tupleSize);
  CkCallback cb (CkReductionTarget(Driver<CentroidData>, treepiecesReportMigrateCountAndMaxVelocity), centroid_driver);
  msg->setCallback(cb);
  this->contribute(msg);
};

template <typename Data>
void TreePiece<Data>::perturb (Real timestep, bool if_flush) {
  if (particles.empty()) return;
  // If tree will be entirely rebuilt, just update particle positions
  // based on the forces calculated from interactions
  if (if_flush) {
    //for (auto && p : particles) {
    //  p.perturb(timestep, readers.ckLocalBranch()->universe.box);
    //}
    flush(readers);
    return;
  }

  // Perturb particles for incremental tree building
  // TODO: Fails with input files with randomly generated particles,
  //       as the initial velocities are too large and particles shoot out
  //       of the universe
  std::vector<Particle> in_particles;
  std::map<int, std::vector<Particle>> out_particles;
  std::vector<int> remainders;
  Key temp = tp_key;
  //const size_t branch_factor = leaves[0]->getBranchFactor();
  const size_t branch_factor = (leaves.size() > 0? leaves[0]->getBranchFactor() :  empty_leaves[0]->getBranchFactor());
  while (temp >= branch_factor) {
    remainders.push_back(temp % branch_factor);
    temp /= branch_factor;
  }
  OrientedBox<Real> tp_box = readers.ckLocalBranch()->universe.box;
  for (int i = remainders.size()-1; i >= 0; i--) {
    for (int dim = 0; dim < 3; dim++) {
      if (remainders[i] & (1 << (2-dim))) tp_box.lesser_corner[dim] = tp_box.center()[dim];
      else tp_box.greater_corner[dim] = tp_box.center()[dim];
    }
  }

  //CkPrintf("=== tp_idx = [%d] ratio = %f\n", local_root->tp_index, migrateRatio);
  // migrateRatio is smaller than threshold, don't rebuild
  // calculate bounding box of TP
  for (auto& particle : particles) {
    //Vector3D<Real> old_position = particle.position;
    //particle.perturb(timestep, readers.ckLocalBranch()->universe.box);
    //CkPrintf("magitude of displacement = %lf\n", (old_position - particle.position).length());
    //CkPrintf("total centroid is (%lf, %lf, %lf)\n", global_root->data.centroid.x, global_root->data.centroid.y, global_root->data.centroid.z);
    OrientedBox<Real> curr_box = tp_box;
    Node<Data>* node = local_root;
    int remainders_index = 0;
    while (!curr_box.contains(particle.position)) {
      //CkPrintf("not under umbrella of node %d with volume %lf\n", node->key, curr_box.volume());
/*      if (node->parent == nullptr) CkPrintf("point (%lf, %lf, %lf) has force (%lf, %lf, %lf) and old position (%lf, %lf, %lf)\n",
              particle.position.x, particle.position.y, particle.position.z,
      leaf->getForce(i).x / .001, leaf->getForce(i).y / .001, leaf->getForce(i).z / .001,
	  old_position.x, old_position.y, old_position.z);*/
      Vector3D<Real> new_point = 2 * curr_box.greater_corner - curr_box.lesser_corner;
      if (remainders[remainders_index] & 4) new_point.x = 2 * curr_box.lesser_corner.x - curr_box.greater_corner.x;
      if (remainders[remainders_index] & 2) new_point.y = 2 * curr_box.lesser_corner.y - curr_box.greater_corner.y;
      if (remainders[remainders_index] & 1) new_point.z = 2 * curr_box.lesser_corner.z - curr_box.greater_corner.z;
      curr_box.grow(new_point);
      remainders_index++;
      node = node->parent;
    }
    //if (node->tp_index >= 0) CkPrintf("node tp_index %d\n", node->tp_index);
    while (node->tp_index < 0) {
      int child = 0;
      Vector3D<Real> mean = curr_box.center();
      for (int dim = 0; dim < 3; dim++) {
        if (particle.position[dim] > mean[dim]) {
          child |= (1 << (2 - dim));
          curr_box.lesser_corner[dim] = mean[dim];
        }
        else curr_box.greater_corner[dim] = mean[dim];
      }
      if (node->type == Node<Data>::Type::RemoteAboveTPKey || node->type == Node<Data>::Type::Remote) {
        CkAbort("flush period too large for initial particle velocities");
      }
      node = node->getChild(child); // move down the tree :)
    }
    if (node == local_root) in_particles.push_back(particle);
    else {
      std::vector<Particle>& particle_vec = out_particles[node->tp_index];
      particle_vec.push_back(particle);
    }
  }
  int count = 0;
  for (auto it = out_particles.begin(); it != out_particles.end(); it++) {
    ParticleMsg* msg = new (it->second.size()) ParticleMsg (it->second.data(), it->second.size());
    count += it->second.size();
    this->thisProxy[it->first].receive(msg);
  }
  particles = in_particles;

}

template <typename Data>
void TreePiece<Data>::flush(CProxy_Reader readers) {
  // debug
  flushed_particles.resize(0);
  flushed_particles.insert(flushed_particles.end(), particles.begin(), particles.end());

  ParticleMsg *msg = new (particles.size()) ParticleMsg(particles.data(), particles.size());
  readers[CkMyPe()].receive(msg);
  particles.resize(0);
  particle_index = 0;
}

template <typename Data>
void TreePiece<Data>::destroy() {
  this->thisProxy[this->thisIndex].ckDestroy();
}

template <typename Data>
void TreePiece<Data>::print(Node<Data>* node) {
  ostringstream oss;
  oss << "tree." << this->thisIndex << ".dot";
  ofstream out(oss.str().c_str());
  CkAssert(out.is_open());
  out << "digraph tree" << this->thisIndex << "{" << endl;
  node->dot(out);
  out << "}" << endl;
  out.close();
}

template <typename Data>
void TreePiece<Data>::output(CProxy_Writer w, CkCallback cb) {
  std::vector<Particle> particles;

  for (const auto& leaf : leaves) {
    particles.insert(particles.end(),
                     leaf->particles(), leaf->particles() + leaf->n_particles);
  }

  std::sort(particles.begin(), particles.end(),
            [](const Particle& left, const Particle& right) {
              return left.order < right.order;
            });

  int particles_per_writer = n_total_particles / CkNumPes();
  if (particles_per_writer * CkNumPes() != n_total_particles)
    ++particles_per_writer;

  int particle_idx = 0;
  while (particle_idx < particles.size()) {
    int writer_idx = particles[particle_idx].order / particles_per_writer;
    int first_particle = writer_idx * particles_per_writer;
    std::vector<Particle> writer_particles;

    while (
      particles[particle_idx].order < first_particle + particles_per_writer
      && particle_idx < particles.size()
      ) {
      writer_particles.push_back(particles[particle_idx]);
      ++particle_idx;
    }

    w[writer_idx].receive(writer_particles, cb);
  }

  if (this->thisIndex != n_treepieces - 1) {
    this->thisProxy[this->thisIndex + 1].output(w, cb);
  }
}

#endif // PARATREET_TREEPIECE_H_
