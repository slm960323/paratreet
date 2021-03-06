#ifndef PARATREET_CACHEMANAGER_H_
#define PARATREET_CACHEMANAGER_H_

#include "paratreet.decl.h"
#include "common.h"
#include "Utility.h"
#include "templates.h"
#include "MultiData.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>

extern CProxy_TreeSpec treespec;

template <typename Data>
class CacheManager : public CBase_CacheManager<Data> {
public:
  std::mutex local_tps_lock;
  Node<Data>* root;
  std::unordered_map<Key, Node<Data>*> local_tps;
  std::set<Key> prefetch_set;
  std::vector<std::vector<Node<Data>*>> delete_at_end;
  CProxy_Resumer<Data> r_proxy;
  Data nodewide_data;

  CacheManager() {
    CkCallback cb(CkIndex_CacheManager<Data>::initialize(), this->thisProxy[this->thisIndex]);
    treespec.check(cb);
  }

  void initialize() {
    Data empty_data;
    SpatialNode<Data> empty_sn (empty_data, 0, false, nullptr, 0);
    root = treespec.ckLocalBranch()->template makeCachedNode<Data>(
        Key(1), Node<Data>::Type::Boundary, empty_sn, nullptr, nullptr); // placeholder
    delete_at_end.resize(CkNumPes());
  }

  ~CacheManager() {
    destroy(false);
  }

  void destroy(bool restore) {
    local_tps.clear();
    prefetch_set.clear();

    for (auto& dae : delete_at_end) {
      for (auto to_delete : dae) {
        delete to_delete;
      }
      dae.resize(0);
    }

    if (root != nullptr) {
      root->triggerFree();
      delete root;
      root = nullptr;
    }

    if (restore) initialize();
  }

  template <typename Visitor>
  void startPrefetch(DPHolder<Data>, CkCallback);
  void startParentPrefetch(DPHolder<Data>, CkCallback);
  void prepPrefetch(Node<Data>*);
  void connect(Node<Data>*, bool);
  void requestNodes(std::pair<Key, int>);
  void makeMsgPerNode(int, std::vector<Node<Data>*>&, std::vector<Particle>&, Node<Data>*);
  void serviceRequest(Node<Data>*, int);
  void recvStarterPack(std::pair<Key, SpatialNode<Data>>* pack, int n, CkCallback);
  void addCache(MultiData<Data>);
  Node<Data>* addCacheHelper(Particle*, int, std::pair<Key, SpatialNode<Data>>*, int, int);
  void restoreData(std::pair<Key, SpatialNode<Data>>);
  void restoreDataHelper(std::pair<Key, SpatialNode<Data>>&, bool);
  void insertNode(Node<Data>*, bool, bool);
  void swapIn(Node<Data>*);
  void process(Key);
};

template <typename Data>
template <typename Visitor>
void CacheManager<Data>::startPrefetch(DPHolder<Data> dp_holder, CkCallback cb) {
  dp_holder.proxy.template prefetch<Visitor>(nodewide_data, this->thisIndex, cb);
}

template <typename Data>
void CacheManager<Data>::startParentPrefetch(DPHolder<Data> dp_holder, CkCallback cb) {
  std::vector<Key> request_list (prefetch_set.begin(), prefetch_set.end());
  dp_holder.proxy.request(request_list.data(), request_list.size(), this->thisIndex, cb);
}

template <typename Data>
void CacheManager<Data>::prepPrefetch(Node<Data>* node) {
  // THIS IS IN LOCK. make faster? TODO
  nodewide_data += node->data;
  Key curr_key = node->key;
  auto branch_factor = node->getBranchFactor();
  while (curr_key > 1) {
    curr_key /= branch_factor;
    prefetch_set.insert(curr_key);
    for (int i = 0; i < branch_factor; i++) {
      prefetch_set.insert(curr_key * branch_factor + i);
    }
  }
}

// Invoked to restore a node in the cached tree structure
// or to store the local roots of TreePieces after the tree is built
template <typename Data>
void CacheManager<Data>::connect(Node<Data>* node, bool should_process) {
#if DEBUG
  CkPrintf("connecting node %d of type %d\n", node->key, node->type);
#endif
  if (node->type == Node<Data>::Type::CachedBoundary) {
    // Invoked internally to update a cached node
    swapIn(node);
    if (should_process) process(node->key);
  } else {
    // Invoked by TreePiece
    if (this->isNodeGroup()) local_tps_lock.lock();

    // Store/connect the incoming TreePiece's local root
    local_tps.insert(std::make_pair(node->key, node));
    prepPrefetch(node);

    if (this->isNodeGroup()) local_tps_lock.unlock();

    if (node->type == Node<Data>::Type::CachedBoundary) {
      CkAbort("local_tps used for a non TP node!");
    }
  }

  // XXX: May need to call process() for dual tree walk
}

template <typename Data>
void CacheManager<Data>::recvStarterPack(std::pair<Key, SpatialNode<Data>>* pack, int n, CkCallback cb) {
  CkPrintf("[CacheManager %d] receiving starter pack, size = %d\n", this->thisIndex, n);

  for (int i = 0; i < n; i++) {
#if DEBUG
    CkPrintf("[CM %d] receiving node %d in starter pack\n", this->thisIndex, pack[i].first);
#endif
    // Restore received data as a tree node in the cache
    // XXX: Can the key ever be equal to a local TP?
    //      If not, the conditional is unnecessary
    if (!local_tps.count(pack[i].first)) {
      restoreDataHelper(pack[i], false);
    }
  }
  if (n == 0) root = local_tps[1];

  this->contribute(cb);
}

template <typename Data>
void CacheManager<Data>::addCache(MultiData<Data> multidata) {
  Node<Data>* top_node = addCacheHelper(multidata.particles.data(), multidata.n_particles, multidata.nodes.data(), multidata.n_nodes, multidata.cm_index);
  process(top_node->key);
}

template <typename Data>
Node<Data>* CacheManager<Data>::addCacheHelper(Particle* particles, int n_particles, std::pair<Key, SpatialNode<Data>>* nodes, int n_nodes, int cm_index) {
#if DEBUG
  CkPrintf("adding cache for top node %d on cm %d\n", nodes[0].first, this->thisIndex);
#endif

  auto first_node_placeholder = root->getDescendant(nodes[0].first);
  if (first_node_placeholder->type == Node<Data>::Type::CachedRemote
      || first_node_placeholder->type == Node<Data>::Type::CachedRemoteLeaf)
  {
    CkAbort("Invalid node placeholder type in CacheManager::addCacheHelper");
  }

  int p_index = 0;
  auto top_type = nodes[0].second.is_leaf ? Node<Data>::Type::CachedRemoteLeaf : Node<Data>::Type::CachedRemote;
  auto first_node = treespec.ckLocalBranch()->template makeCachedNode<Data>(nodes[0].first, top_type, nodes[0].second, first_node_placeholder->parent, particles);
  first_node->cm_index = cm_index;
  insertNode(first_node, false, false);
  auto branch_factor = first_node->getBranchFactor();
  for (int j = 1; j < n_nodes; j++) {
    auto && new_key = nodes[j].first;
    auto && spatial_node = nodes[j].second;
    auto curr_parent = first_node->getDescendant(new_key / branch_factor);
    auto type = spatial_node.is_leaf ? Node<Data>::Type::CachedRemoteLeaf : Node<Data>::Type::CachedRemote;
    auto node = treespec.ckLocalBranch()->template makeCachedNode<Data>(new_key, type, spatial_node, curr_parent, &particles[p_index]);
    node->cm_index = cm_index;
    if (node->is_leaf) p_index += spatial_node.n_particles;
    insertNode(node, false, true);
  }
  swapIn(first_node);
  return first_node;
}

template <typename Data>
void CacheManager<Data>::requestNodes(std::pair<Key, int> param) {
  Key key = param.first;
  Key temp = key;
  while (!local_tps.count(temp)) temp /= root->getBranchFactor();
  Node<Data>* node = local_tps[temp]->getDescendant(key);
  if (!node) {
    CkPrintf("CacheManager::requestNodes: node not found for key %lu on cm %d\n", param.first, this->thisIndex);
    CkAbort("CacheManager::requestNodes: node not found");
  }
  serviceRequest(node, param.second);
}

template <typename Data>
void CacheManager<Data>::makeMsgPerNode(int start_depth, std::vector<Node<Data>*>& sending_nodes, std::vector<Particle>& sending_particles, Node<Data>* to_process)
{
  auto config = treespec.ckLocalBranch()->getConfiguration();
  sending_nodes.push_back(to_process);
  if (to_process->type == Node<Data>::Type::Leaf) {
    std::copy(to_process->particles(), to_process->particles() + to_process->n_particles, std::back_inserter(sending_particles));
  }
  if (to_process->depth + 1 < start_depth + config.cache_share_depth) {
    for (int i = 0; i < to_process->n_children; i++) {
      Node<Data>* child = to_process->getChild(i);
      makeMsgPerNode(start_depth, sending_nodes, sending_particles, child);
    }
  }
}

template <typename Data>
void CacheManager<Data>::serviceRequest(Node<Data>* node, int cm_index) {
  if (cm_index == this->thisIndex) return; // you'll get it later!
  std::vector<Node<Data>*> sending_nodes;
  std::vector<Particle> sending_particles;
  int start_depth = node->depth;
  makeMsgPerNode(node->depth, sending_nodes, sending_particles, node);
  MultiData<Data> multidata (sending_particles.data(), sending_particles.size(), sending_nodes.data(), sending_nodes.size(), this->thisIndex);
  this->thisProxy[cm_index].addCache(multidata);
}

template <typename Data>
void CacheManager<Data>::restoreData(std::pair<Key, SpatialNode<Data>> param) {
  restoreDataHelper(param, true);
}

template <typename Data>
void CacheManager<Data>::restoreDataHelper(std::pair<Key, SpatialNode<Data>>& param, bool should_process) {
#if DEBUG
  if (!should_process) CkPrintf("restoring data for node %d\n", param.first);
#endif
  Key key = param.first;
  auto branch_factor = root->getBranchFactor();
  Node<Data>* parent = (key > 1) ? root->getDescendant(key / branch_factor) : nullptr;
  auto node = treespec.ckLocalBranch()->template makeCachedNode<Data>(key,
      Node<Data>::Type::CachedBoundary, param.second, parent, nullptr);
  insertNode(node, true, false);
  connect(node, should_process);
}

template <typename Data>
void CacheManager<Data>::swapIn(Node<Data>* to_swap) {
  if (to_swap->key > 1) {
    auto which_child = to_swap->key % to_swap->getBranchFactor();
    to_swap = to_swap->parent->exchangeChild(which_child, to_swap);
  }
  else {
    std::swap(root, to_swap);
  }
  delete_at_end[CkMyRank()].push_back(to_swap);
}

template <typename Data>
void CacheManager<Data>::insertNode(Node<Data>* node, bool above_tp, bool should_swap) {
#if DEBUG
  CkPrintf("inserting node %d of type %d with %d children\n", node->key, node->type, node->n_children);
#endif
  for (int i = 0; i < node->n_children; i++) {
    Node<Data>* new_child = nullptr;
    Key child_key = node->key * node->getBranchFactor() + i;
    bool add_placeholder = false;
    if (above_tp) {
      auto it = local_tps.find(child_key);
      if (it != local_tps.end()) {
        new_child = it->second;
        new_child->parent = node;
      }
      else {
        add_placeholder = true;
      }
    }
    if (!above_tp || add_placeholder) {
      auto type = (above_tp) ? Node<Data>::Type::RemoteAboveTPKey : Node<Data>::Type::Remote;
      Data empty_data;
      SpatialNode<Data> empty_sn (empty_data, 0, false, nullptr, 0);
      new_child = treespec.ckLocalBranch()->makeCachedNode(child_key, type, empty_sn, node, nullptr); // placeholder
      if (!above_tp) new_child->cm_index = node->cm_index;
    }
    node->exchangeChild(i, new_child);
  }
  if (should_swap) swapIn(node);
}

template <typename Data>
void CacheManager<Data>::process(Key key) {
  if (!this->isNodeGroup()) r_proxy[this->thisIndex].process (key);
  else for (int i = 0; i < CkNodeSize(0); i++) {
    r_proxy[this->thisIndex * CkNodeSize(0) + i].process(key);
  }
}

#endif //PARATREET_CACHEMANAGER_H_
