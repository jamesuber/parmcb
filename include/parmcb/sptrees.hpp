#ifndef PARMCB_SPTREES_HPP_
#define PARMCB_SPTREES_HPP_

//    Copyright (C) Dimitrios Michail 2019 - 2021.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          https://www.boost.org/LICENSE_1_0.txt)

#include <iostream>

#include <boost/throw_exception.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/property_map/function_property_map.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_concepts.hpp>
#include <boost/graph/graph_utility.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/serialization/vector.hpp>

#include <parmcb/config.hpp>
#include <parmcb/detail/lex_dijkstra.hpp>
#include <parmcb/detail/util.hpp>

#include <parmcb/forestindex.hpp>
#include <parmcb/spvecgf2.hpp>

#include <memory>
#include <stack>
#include <functional>

#ifdef PARMCB_HAVE_TBB
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#endif

namespace parmcb {

    template<class Graph, class WeightMap> class SPNode;
    template<class Graph, class WeightMap> class SPTree;
    template<class Graph, class WeightMap, class T> struct SPSubtree;
    template<class Graph, class WeightMap, bool ParallelUsingTBB> class SPTrees;
    template<class Graph, class WeightMap> class CandidateCycle;
    template<class Graph> struct SerializableCandidateCycle;
    template<class Graph, class WeightMap> struct SerializableMinOddCycle;
    template<class Graph, class WeightMap> struct SerializableMinOddCycleMinOp;

    template<class Graph, class WeightMap>
    class SPNode {
    public:
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        SPNode() :
                _vertex(), _parity(false), _weight(WeightType()), _pred(), _has_pred(false) {
        }

        SPNode(Vertex vertex, WeightType weight) :
                _vertex(vertex), _parity(false), _weight(WeightType()), _pred(), _has_pred(false) {
        }

        SPNode(Vertex vertex, WeightType weight, const Edge &pred) :
                _vertex(vertex), _parity(false), _weight(weight), _pred(pred), _has_pred(true) {
        }

        void add_child(std::shared_ptr<SPNode<Graph, WeightMap>> c) {
            _children.push_back(c);
        }

        std::vector<std::shared_ptr<SPNode<Graph, WeightMap>>>& children() {
            return _children;
        }

        Vertex& vertex() {
            return _vertex;
        }

        bool& parity() {
            return _parity;
        }

        WeightType& weight() {
            return _weight;
        }

        const Edge& pred() {
            return _pred;
        }

        bool has_pred() {
            return _has_pred;
        }

    private:
        Vertex _vertex;
        bool _parity;
        WeightType _weight;
        Edge _pred;
        bool _has_pred;
        std::vector<std::shared_ptr<SPNode<Graph, WeightMap>>> _children;
    };

    template<class Graph, class WeightMap, class T>
    struct SPSubtree {
        T info;
        std::shared_ptr<SPNode<Graph, WeightMap>> root;

        SPSubtree(T info, std::shared_ptr<SPNode<Graph, WeightMap>> root) :
                info(info), root(root) {
        }
    };

    template<class Graph, class WeightMap>
    class SPTree {
    public:
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename boost::graph_traits<Graph>::vertex_iterator VertexIt;
        typedef typename boost::property_map<Graph, boost::vertex_index_t>::type VertexIndexMapType;
        typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        SPTree(std::size_t id, const Graph &g, const VertexIndexMapType& index_map, const WeightMap &weight_map, const Vertex &source) :
                _id(id), _g(g), _weight_map(weight_map), _index_map(index_map), _source(
                        source), _tree_node_map(boost::num_vertices(g)), _first_in_path(boost::num_vertices(g)) {
            initialize();
        }

        void update_parities(const std::set<Edge> &edges) {
            std::stack<SPSubtree<Graph, WeightMap, bool>> stack;
            stack.emplace(false, _root);

            while (!stack.empty()) {
                SPSubtree<Graph, WeightMap, bool> r = stack.top();
                stack.pop();

                r.root->parity() = r.info;
                for (auto c : r.root->children()) {
                    bool is_signed = edges.find(c->pred()) != edges.end();
                    stack.emplace(SPSubtree<Graph, WeightMap, bool> { static_cast<bool>(r.info ^ is_signed), c });
                }
            }
        }

        std::shared_ptr<SPNode<Graph, WeightMap>> node(const Vertex &v) const {
            return _tree_node_map[_index_map[v]];
        }

        const Vertex& source() const {
            return _source;
        }

        const Graph& graph() const {
            return _g;
        }

        const std::size_t id() const {
            return _id;
        }

        const Vertex first(const Vertex &v) {
            auto vindex = _index_map[v];
            return _first_in_path[vindex];
        }

        template<class EdgeIterator>
        std::vector<CandidateCycle<Graph, WeightMap>> create_candidate_cycles(EdgeIterator begin,
                EdgeIterator end) const {
            // collect tree edges
            std::set<Edge> tree_edges;
            VertexIt vi, viend;
            for (boost::tie(vi, viend) = boost::vertices(_g); vi != viend; ++vi) {
                auto v = *vi;
                auto vindex = _index_map[v];
                std::shared_ptr<SPNode<Graph, WeightMap>> n = _tree_node_map[vindex];
                if (n != nullptr && n->has_pred()) {
                    tree_edges.insert(n->pred());
                }
            }

            // loop over (non-tree) provided edges and create candidate cycles
            std::vector<CandidateCycle<Graph, WeightMap>> cycles;
            for (EdgeIterator it = begin; it != end; it++) {
                Edge e = *it;
                if (tree_edges.find(e) != tree_edges.end()) {
                    continue;
                }

                // non-tree edge
                std::shared_ptr<SPNode<Graph, WeightMap>> v = node(boost::source(e, _g));
                if (v == nullptr) {
                    continue;
                }
                std::shared_ptr<SPNode<Graph, WeightMap>> u = node(boost::target(e, _g));
                if (u == nullptr) {
                    continue;
                }

                if (_first_in_path[_index_map[v->vertex()]] == _first_in_path[_index_map[u->vertex()]]) {
                    // shortest paths start with the same vertex, discard
                    continue;
                }

                WeightType cycle_weight = boost::get(_weight_map, e) + v->weight() + u->weight();
                cycles.emplace_back(_id, e, cycle_weight);
            }
            return cycles;
        }

        std::vector<CandidateCycle<Graph, WeightMap>> create_candidate_cycles() const {
            auto itPair = boost::edges(_g);
            return create_candidate_cycles(itPair.first, itPair.second);
        }

        std::vector<SerializableCandidateCycle<Graph>> create_serializable_candidate_cycles(
                const ForestIndex<Graph> &forest_index) {
            // collect tree edges
            std::set<Edge> tree_edges;
            VertexIt vi, viend;
            for (boost::tie(vi, viend) = boost::vertices(_g); vi != viend; ++vi) {
                auto v = *vi;
                auto vindex = _index_map[v];
                std::shared_ptr<SPNode<Graph, WeightMap>> n = _tree_node_map[vindex];
                if (n != nullptr && n->has_pred()) {
                    tree_edges.insert(n->pred());
                }
            }

            // loop over all non-tree edges and create candidate cycles
            std::vector<SerializableCandidateCycle<Graph>> cycles;
            for (const auto &e : boost::make_iterator_range(boost::edges(_g))) {
                if (tree_edges.find(e) != tree_edges.end()) {
                    continue;
                }

                // non-tree edge
                std::shared_ptr<SPNode<Graph, WeightMap>> v = node(boost::source(e, _g));
                if (v == nullptr) {
                    continue;
                }
                std::shared_ptr<SPNode<Graph, WeightMap>> u = node(boost::target(e, _g));
                if (u == nullptr) {
                    continue;
                }

                if (_first_in_path[_index_map[v->vertex()]] == _first_in_path[_index_map[u->vertex()]]) {
                    // shortest paths start with the same vertex, discard
                    continue;
                }

                cycles.emplace_back(_source, forest_index(e));
            }

            return cycles;
        }

    private:
        const std::size_t _id;
        const Graph &_g;
        const WeightMap &_weight_map;
        const VertexIndexMapType &_index_map;
        const Vertex _source;

        /*
         * Shortest path tree root
         */
        std::shared_ptr<SPNode<Graph, WeightMap>> _root;
        /*
         * Map from vertex to shortest path tree node
         */
        std::vector<std::shared_ptr<SPNode<Graph, WeightMap>>> _tree_node_map;
        /*
         * First vertex in shortest path from root to a vertex.
         */
        std::vector<Vertex> _first_in_path;

        void initialize() {
            // run shortest path
            std::vector<WeightType> dist(boost::num_vertices(_g), (std::numeric_limits<WeightType>::max)());
            boost::function_property_map<parmcb::detail::VertexIndexFunctor<Graph, WeightType>, Vertex, WeightType&> dist_map(
                    parmcb::detail::VertexIndexFunctor<Graph, WeightType>(dist, _index_map));
            std::vector<std::tuple<bool, Edge>> pred(boost::num_vertices(_g), std::make_tuple(false, Edge()));
            boost::function_property_map<parmcb::detail::VertexIndexFunctor<Graph, std::tuple<bool, Edge>>, Vertex,
                    std::tuple<bool, Edge>&> pred_map(
                    parmcb::detail::VertexIndexFunctor<Graph, std::tuple<bool, Edge> >(pred, _index_map));
            lex_dijkstra(_g, _weight_map, _source, dist_map, pred_map);

            // create tree nodes and mapping
            VertexIt vi, viend;
            for (boost::tie(vi, viend) = boost::vertices(_g); vi != viend; ++vi) {
                auto v = *vi;
                auto vindex = _index_map[v];
                auto p = boost::get(pred_map, v);
                if (v == _source) {
                    _tree_node_map[vindex] = std::shared_ptr<SPNode<Graph, WeightMap>>(
                            new SPNode<Graph, WeightMap>(v, dist[vindex]));
                    _root = _tree_node_map[vindex];
                } else if (std::get<0>(p)) {
                    Edge e = std::get<1>(p);
                    _tree_node_map[vindex] = std::shared_ptr<SPNode<Graph, WeightMap>>(
                            new SPNode<Graph, WeightMap>(v, dist[vindex], e));
                }
            }

            // link tree nodes
            for (boost::tie(vi, viend) = boost::vertices(_g); vi != viend; ++vi) {
                auto v = *vi;
                auto p = boost::get(pred_map, v);
                if (std::get<0>(p)) {
                    auto e = std::get<1>(p);
                    auto u = boost::opposite(e, v, _g);
                    auto vindex = _index_map[v];
                    auto uindex = _index_map[u];
                    _tree_node_map[uindex]->add_child(_tree_node_map[vindex]);
                }
            }

            // compute first in path
            compute_first_in_path();
        }

        void compute_first_in_path() {
            std::stack<SPSubtree<Graph, WeightMap, Vertex>> stack;
            stack.emplace(_source, _root);

            while (!stack.empty()) {
                SPSubtree<Graph, WeightMap, Vertex> r = stack.top();
                stack.pop();

                if (r.root == _root) {
                    auto v = r.root->vertex();
                    auto vindex = _index_map[v];
                    _first_in_path[vindex] = v;
                    for (auto c : r.root->children()) {
                        stack.emplace(SPSubtree<Graph, WeightMap, Vertex> { static_cast<Vertex>(c->vertex()), c });
                    }
                } else {
                    auto v = r.root->vertex();
                    auto vindex = _index_map[v];
                    _first_in_path[vindex] = r.info;
                    for (auto c : r.root->children()) {
                        stack.emplace(SPSubtree<Graph, WeightMap, Vertex> { static_cast<Vertex>(r.info), c });
                    }
                }
            }
        }

    };

    template<class Graph, class WeightMap>
    class CandidateCycle {
    public:
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        CandidateCycle(std::size_t tree, const Edge &e, WeightType weight) :
                _tree(tree), _e(e), _weight(weight) {
        }

        CandidateCycle(const CandidateCycle &c) :
                _tree(c._tree), _e(c._e), _weight(c._weight) {
        }

        CandidateCycle& operator=(const CandidateCycle &other) {
            if (this != &other) {
                _tree = other._tree;
                _e = other._e;
                _weight = other._weight;
            }
            return *this;
        }

        std::size_t tree() const {
            return _tree;
        }

        const Edge& edge() const {
            return _e;
        }

        const WeightType& weight() const {
            return _weight;
        }

    private:
        std::size_t _tree;
        Edge _e;
        WeightType _weight;
    };

    template<class Graph>
    struct SerializableCandidateCycle {
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename ForestIndex<Graph>::size_type Edge;

        SerializableCandidateCycle() {
        }

        SerializableCandidateCycle(Vertex v, Edge e) :
                v(v), e(e) {
        }

        template<typename Archive>
        void serialize(Archive &ar, const unsigned) {
            ar & v;
            ar & e;
        }

        Vertex v;
        Edge e;
    };

    template<class Graph, class WeightMap>
    struct SerializableMinOddCycle {
        typedef typename ForestIndex<Graph>::size_type Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        SerializableMinOddCycle() :
                exists(false) {
        }

        SerializableMinOddCycle(std::vector<Edge> edges, WeightType weight, bool exists) :
                edges(edges), weight(weight), exists(exists) {
        }

        SerializableMinOddCycle(const SerializableMinOddCycle<Graph, WeightMap> &c) :
                edges(c.edges), weight(c.weight), exists(c.exists) {
        }

        SerializableMinOddCycle<Graph, WeightMap>& operator=(const SerializableMinOddCycle<Graph, WeightMap> &other) {
            if (this != &other) {
                edges = other.edges;
                weight = other.weight;
                exists = other.exists;
            }
            return *this;
        }

        template<typename Archive>
        void serialize(Archive &ar, const unsigned) {
            ar & edges;
            ar & weight;
            ar & exists;
        }

        std::vector<Edge> edges;
        WeightType weight;
        bool exists;
    };

    template<class Graph, class WeightMap>
    struct SerializableMinOddCycleMinOp {

        const SerializableMinOddCycle<Graph, WeightMap>& operator()(
                const SerializableMinOddCycle<Graph, WeightMap> &lhs,
                const SerializableMinOddCycle<Graph, WeightMap> &rhs) const {
            if (!lhs.exists || !rhs.exists) {
                if (lhs.exists) {
                    return lhs;
                } else {
                    return rhs;
                }
            }
            // both valid, compare
            if (lhs.weight < rhs.weight) {
                return lhs;
            }
            return rhs;
        }

    };

    template<class Graph, class WeightMap>
    class CandidateCycleToSerializableConverter {
    public:
        CandidateCycleToSerializableConverter(const std::vector<parmcb::SPTree<Graph, WeightMap>> &trees,
                const ForestIndex<Graph> &forest_index) :
                trees(trees), forest_index(forest_index) {
        }

        SerializableCandidateCycle<Graph> operator()(const CandidateCycle<Graph, WeightMap> &cycle) const {
            return SerializableCandidateCycle<Graph>(trees.at(cycle.tree()).source(), forest_index(cycle.edge()));
        }

    private:
        const std::vector<parmcb::SPTree<Graph, WeightMap>> &trees;
        const ForestIndex<Graph> &forest_index;
    };

    template<class Graph, class WeightMap>
    class CandidateCycleBuilder {
    public:
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        CandidateCycleBuilder(const Graph &g, const WeightMap &weight_map) :
                g(g), weight_map(weight_map) {
        }

        std::tuple<std::set<Edge>, WeightType, bool> operator()(const std::vector<parmcb::SPTree<Graph, WeightMap>> &trees,
                const CandidateCycle<Graph, WeightMap> &c, const std::set<Edge> &signed_edges, bool use_weight_limit,
                WeightType weight_limit) const {

            std::shared_ptr<SPNode<Graph, WeightMap>> v = trees[c.tree()].node(boost::source(c.edge(), g));
            std::shared_ptr<SPNode<Graph, WeightMap>> u = trees[c.tree()].node(boost::target(c.edge(), g));

            Edge e = c.edge();
            if (v->parity() ^ u->parity() ^ (signed_edges.find(e) != signed_edges.end())) {
                // odd cycle, validate
                bool valid = true;
                WeightType cycle_weight = boost::get(weight_map, e);
                std::set<Edge> result;
                result.insert(e);

                if (use_weight_limit && cycle_weight > weight_limit) {
                    return std::make_tuple(std::set<Edge> { }, 0.0, false);
                }

                // first part
                Vertex w = boost::source(c.edge(), g);
                std::shared_ptr<SPNode<Graph, WeightMap>> ws = trees[c.tree()].node(w);
                while (ws->has_pred()) {
                    Edge a = ws->pred();
                    if (result.insert(a).second == false) {
                        valid = false;
                        break;
                    }
                    cycle_weight += boost::get(weight_map, a);
                    if (use_weight_limit && cycle_weight > weight_limit) {
                        valid = false;
                        break;
                    }
                    w = boost::opposite(a, w, g);
                    ws = trees[c.tree()].node(w);
                }

                if (!valid) {
                    return std::make_tuple(std::set<Edge> { }, 0.0, false);
                }

                // second part
                w = boost::target(c.edge(), g);
                ws = trees[c.tree()].node(w);
                while (ws->has_pred()) {
                    Edge a = ws->pred();
                    if (result.insert(a).second == false) {
                        valid = false;
                        break;
                    }
                    cycle_weight += boost::get(weight_map, a);
                    if (use_weight_limit && cycle_weight > weight_limit) {
                        valid = false;
                        break;
                    }
                    w = boost::opposite(a, w, g);
                    ws = trees[c.tree()].node(w);
                }

                if (!valid) {
                    return std::make_tuple(std::set<Edge> { }, 0.0, false);
                }

                return std::make_tuple(result, cycle_weight, true);
            }
            return std::make_tuple(std::set<Edge> { }, 0.0, false);
        }

    private:
        const Graph &g;
        const WeightMap &weight_map;
    };

    template<class Graph, class WeightMap, bool ParallelUsingTBB>
    class ShortestOddCycleLookup {
    public:
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
        typedef typename boost::property_traits<WeightMap>::value_type WeightType;

        ShortestOddCycleLookup(const Graph &g, const WeightMap &weight_map,
                std::vector<parmcb::SPTree<Graph, WeightMap>> &trees,
                std::vector<parmcb::CandidateCycle<Graph, WeightMap>> &cycles, bool sorted_cycles) :
                g(g), weight_map(weight_map), candidate_cycle_builder(g, weight_map), trees(trees), cycles(cycles), sorted_cycles(
                        sorted_cycles) {
        }

        std::tuple<std::set<Edge>, WeightType, bool> operator()(const std::set<Edge> &edges) {
            return compute_shortest_odd_cycle(edges);
        }

    private:

        template<bool is_tbb_enabled = ParallelUsingTBB>
        std::tuple<std::set<Edge>, WeightType, bool> compute_shortest_odd_cycle(const std::set<Edge> &edges,
                typename std::enable_if<!is_tbb_enabled>::type* = 0) {

            for (std::size_t i = 0; i < trees.size(); i++) {
                trees[i].update_parities(edges);
            }

            std::tuple<std::set<Edge>, WeightType, bool> min;

            for (CandidateCycle<Graph, WeightMap> c : cycles) {
                std::tuple<std::set<Edge>, WeightType, bool> cc = candidate_cycle_builder(trees, c, edges,
                        std::get<2>(min), std::get<1>(min));

                if (std::get<2>(cc)) {
                    if (sorted_cycles) {
                        return cc;
                    }

                    if (!std::get<2>(min)) {
                        min = cc;
                    } else {
                        if (std::get<1>(cc) < std::get<1>(min)) {
                            min = cc;
                        }
                    }
                }
            }
            return min;
        }

        template<bool is_tbb_enabled = ParallelUsingTBB>
        std::tuple<std::set<Edge>, WeightType, bool> compute_shortest_odd_cycle(const std::set<Edge> &edges,
                typename std::enable_if<is_tbb_enabled>::type* = 0) {

            tbb::parallel_for(tbb::blocked_range<std::size_t>(0, trees.size()),
                    [&](const tbb::blocked_range<std::size_t> &r) {
                        for (std::size_t i = r.begin(); i != r.end(); ++i) {
                            trees[i].update_parities(edges);
                        }
                    });

            std::less<WeightType> compare = std::less<WeightType>();
            typedef std::tuple<std::set<Edge>, WeightType, bool> cycle_t;
            auto cycle_min = [compare](const cycle_t &c1, const cycle_t &c2) {
                if (!std::get<2>(c1) || !std::get<2>(c2)) {
                    if (std::get<2>(c1)) {
                        return c1;
                    } else {
                        return c2;
                    }
                }
                // both valid, compare
                if (!compare(std::get<1>(c2), std::get<1>(c1))) {
                    return c1;
                }
                return c2;
            };

            return tbb::parallel_reduce(tbb::blocked_range<std::size_t>(0, cycles.size()),
                    std::make_tuple(std::set<Edge>(), (std::numeric_limits<WeightType>::max)(), false),
                    [&](tbb::blocked_range<std::size_t> r, auto running_min) {
                        for (std::size_t i = r.begin(); i < r.end(); i++) {
                            auto c = cycles[i];
                            auto cc = candidate_cycle_builder(trees, c, edges, std::get<2>(running_min),
                                    std::get<1>(running_min));
                            if (std::get<2>(cc)) {
                                if (!std::get<2>(running_min) || compare(std::get<1>(cc), std::get<1>(running_min))) {
                                    running_min = cc;
                                }
                            }
                        }
                        return running_min;
                    },
                    cycle_min);
        }

        const Graph &g;
        const WeightMap &weight_map;
        const CandidateCycleBuilder<Graph, WeightMap> candidate_cycle_builder;
        std::vector<parmcb::SPTree<Graph, WeightMap>> &trees;
        std::vector<parmcb::CandidateCycle<Graph, WeightMap>> &cycles;
        bool sorted_cycles;
    };

} // parmcb

#endif
