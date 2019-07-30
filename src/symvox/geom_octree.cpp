
//=============================================================
//  This file is part of the SymVox (Symmetry Voxelization) software
//  Copyright (C) 2016 by CRS4 Visual Computing Group, Pula, Italy
//
//  For more information, visit the CRS4 Visual Computing Group 
//  web pages at http://vic.crs4.it
//
//  This file may be used under the terms of the GNU General Public
//  License as published by the Free Software Foundation and appearing
//  in the file LICENSE included in the packaging of this file.
//
//  CRS4 reserves all rights not expressly granted herein.
//  
//  This file is provided AS IS with NO WARRANTY OF ANY KIND, 
//  INCLUDING THE WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS 
//  FOR A PARTICULAR PURPOSE.
//=============================================================

#include <queue>
#include <stack>
#include <set>
#include <utility>
#include <omp.h>
#include <memory>

#include <fstream>

#if BUILD_LASPARSER
#include <liblas/liblas.hpp>
#endif

#include<sl/external_array.hpp>

#include <symvox/geom_octree.hpp>
#include <symvox/test_triangle_box.hpp>
#include <symvox/util.hpp>

GeomOctree::GeomOctree(Scene * scene) : Octree(scene) {
	_state = S_EMPTY;
}


GeomOctree::GeomOctree(const GeomOctree &other) : Octree (other) {
	_state = other._state;
	_stats = other._stats;
	_data.reserve(other._data.size());
	_data = other._data;
}

void GeomOctree::buildSVOFromPoints(std::string fileName, unsigned int levels, sl::aabox3d bbox, bool internalCall, std::vector< sl::point3d > * leavesCenters) {
    if (!internalCall) printf("* Building SVO... "); fflush(stdout);
#if BUILD_LASPARSER
	//_bbox = sl::conv_to<sl::aabox3f>::from(bbox);
	auto minD = bbox[0], maxD = bbox[1];
	sl::point3f minF = sl::point3f(minD[0], minD[1], minD[2]);
	sl::point3f maxF = sl::point3f(maxD[0], maxD[1], maxD[2]);
	_bbox = sl::aabox3f(minF, maxF);

    _levels = levels;
    sl::vector3f sides =_bbox.half_side_lengths() * 2.0f;
    _rootSide = sl::max(sl::max(sides[0], sides[1]), sides[2]);

    _data.resize(_levels);

    Node root;
    _data[0].push_back(root);

    struct QueueItem {
        QueueItem(id_t id, sl::uint8_t l, sl::point3d c) : nodeID(id), level(l), center(c) {}
        id_t nodeID;
        sl::uint8_t level;
        sl::point3d center;
    };
    sl::point3d childrenCenters[8];
    std::stack<QueueItem> queue;

    if(!internalCall) _clock.restart();

    // Prepare for reading the file
    std::ifstream ifs;
    ifs.open(fileName, std::ios::in | std::ios::binary);

    liblas::ReaderFactory f;
    liblas::Reader reader = f.CreateWithStream(ifs);

    int numPoints = reader.GetHeader().GetPointRecordsCount();
    int stepLogger = (int)round(numPoints / 10.f);

    // For every point
    unsigned int iPoint = 0;
    while (reader.ReadNextPoint()) {
        liblas::Point const& p = reader.GetPoint();

        if (!internalCall && (iPoint % stepLogger == 0)) {
            printf("%.0f%%..", round(100.f * (iPoint / (float)numPoints)));
            fflush(stdout);
        }

        // Push the root node to a queue
        queue.push(QueueItem(0, 0, bbox.center()));

        // Traverse through all nodes in the tree that intersect with the triangle, starting at the root
        while (!queue.empty()) {
            const QueueItem qi = queue.top(); queue.pop();

            // Store of the position (center) of all child nodes of the current node
            double k = getHalfSideD(qi.level + 1);
            childrenCenters[PXPYPZ] = qi.center + sl::vector3d(+k, +k, +k);
            childrenCenters[PXPYNZ] = qi.center + sl::vector3d(+k, +k, -k);
            childrenCenters[PXNYPZ] = qi.center + sl::vector3d(+k, -k, +k);
            childrenCenters[PXNYNZ] = qi.center + sl::vector3d(+k, -k, -k);
            childrenCenters[NXPYPZ] = qi.center + sl::vector3d(-k, +k, +k);
            childrenCenters[NXPYNZ] = qi.center + sl::vector3d(-k, +k, -k);
            childrenCenters[NXNYPZ] = qi.center + sl::vector3d(-k, -k, +k);
            childrenCenters[NXNYNZ] = qi.center + sl::vector3d(-k, -k, -k);

            // Traverse through all child nodes
            Node & node = _data[qi.level][qi.nodeID];
            for (int i = 7; i >= 0; --i) {
                // If the point lies inside this child...
                if ((p.GetX() >= childrenCenters[i][0] -k && p.GetX() < childrenCenters[i][0] +k)
                 && (p.GetY() >= childrenCenters[i][1] -k && p.GetY() < childrenCenters[i][1] +k)
                 && (p.GetZ() >= childrenCenters[i][2] -k && p.GetZ() < childrenCenters[i][2] +k)) {
                    // Mark the child mask
                    node.setChildBit(i);
                    // If there is no child node inserted yet, and it's not a leaf, insert a child node
                    if (!node.existsChildPointer(i) && (qi.level < (_levels - 1))) {
                        node.children[i] = (id_t)_data[qi.level + 1].size();
                        _data[qi.level + 1].emplace_back();
                        _nNodes++;
                        if (leavesCenters!=NULL && (qi.level == (_levels - 2))) leavesCenters->push_back(childrenCenters[i]);
                    }
                    // If this child is not a leaf, continue intersecting its children in a future iteration
                    if((qi.level+1U) < _levels) queue.push(QueueItem(node.children[i], qi.level + 1, childrenCenters[i]));
                }
            }
        }
        iPoint++;
    }

    cleanEmptyNodes();
    if (!internalCall) _stats.buildSVOTime = _clock.elapsed();

    // compute NVoxels
    for (id_t i = 0; i < _data[_levels - 1].size(); ++i) {
        _nVoxels += _data[_levels - 1][i].getNChildren();
    }

    _state = S_SVO;
    _stats.nNodesSVO = _nNodes;
    _stats.nNodesLastLevSVO = _data[_levels - 1].size();
    _stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
    _stats.nTotalVoxels = _nVoxels;

    if(!internalCall) printf("OK! [%s]\n",sl::human_readable_duration(_stats.buildSVOTime).c_str());
#endif
}

void GeomOctree::buildSVO(unsigned int levels, sl::aabox3d bbox, bool internalCall, std::vector< sl::point3d > * leavesCenters, bool putMaterialIdInLeaves) {

    if (!internalCall) printf("* Building SVO... ");
    fflush(stdout);

    //_bbox = sl::conv_to<sl::aabox3f>::from(bbox);
    auto minD = bbox[0], maxD = bbox[1];
    sl::point3f minF = sl::point3f(minD[0], minD[1], minD[2]);
    sl::point3f maxF = sl::point3f(maxD[0], maxD[1], maxD[2]);
    _bbox = sl::aabox3f(minF, maxF);

    _levels = levels;
    sl::vector3f sides = _bbox.half_side_lengths() * 2.0f;
    _rootSide = sl::max(sl::max(sides[0], sides[1]), sides[2]);

    _data.resize(_levels);

    Node root;
    _data[0].push_back(root);

    struct QueueItem {
        QueueItem(id_t id, sl::uint8_t l, sl::point3d c) : nodeID(id), level(l), center(c) {}

        id_t nodeID;
        sl::uint8_t level;
        sl::point3d center;
    };
    sl::point3d childrenCenters[8];
    std::stack<QueueItem> queue;

    if (!internalCall) _clock.restart();

    int stepLogger = (int) round(_scene->getNRawTriangles() / 10.f);
    // For every triangle...
    for (std::size_t iTri = 0; iTri < _scene->getNRawTriangles(); iTri++) {
        if (!internalCall && (iTri % stepLogger == 0)) {
            printf("%.0f%%..", round(100.f * (iTri / (float) _scene->getNRawTriangles())));
            fflush(stdout);
        }
        unsigned int triMatId;
        if (putMaterialIdInLeaves) triMatId = (unsigned int) _scene->getTriangleMaterialId(iTri);

        // Push the root node to a queue
        queue.push(QueueItem(0, 0, bbox.center()));

        // Traverse through all nodes in the tree that intersect with the triangle, starting at the root
        while (!queue.empty()) {
            const QueueItem qi = queue.top();
            queue.pop();

            // Store of the position (center) of all child nodes of the current node
            double k = getHalfSideD(qi.level + 1);
            childrenCenters[PXPYPZ] = qi.center + sl::vector3d(+k, +k, +k);
            childrenCenters[PXPYNZ] = qi.center + sl::vector3d(+k, +k, -k);
            childrenCenters[PXNYPZ] = qi.center + sl::vector3d(+k, -k, +k);
            childrenCenters[PXNYNZ] = qi.center + sl::vector3d(+k, -k, -k);
            childrenCenters[NXPYPZ] = qi.center + sl::vector3d(-k, +k, +k);
            childrenCenters[NXPYNZ] = qi.center + sl::vector3d(-k, +k, -k);
            childrenCenters[NXNYPZ] = qi.center + sl::vector3d(-k, -k, +k);
            childrenCenters[NXNYNZ] = qi.center + sl::vector3d(-k, -k, -k);

            // Traverse through all child nodes
            Node &node = _data[qi.level][qi.nodeID];
            for (int i = 7; i >= 0; --i) {
                // If the triangle intersects with this child...
                if (_scene->getTrianglePtr(iTri) != NULL &&
                    testTriBox(childrenCenters[i], k, _scene->getTrianglePtr(iTri))) {
                    // Mark the child mask
                    node.setChildBit(i);
                    // If there is no child node inserted yet, and it's not a leaf, insert a child node
                    if (!node.existsChildPointer(i) && (qi.level < (_levels - 1))) {
                        node.children[i] = (id_t) _data[qi.level + 1].size();
                        _data[qi.level + 1].emplace_back();
                        _nNodes++;
                        if (leavesCenters != NULL && (qi.level == (_levels - 2)))
                            leavesCenters->push_back(childrenCenters[i]);
                    }
                    // If this child is not a leaf, continue intersecting its children in a future iteration
                    if ((qi.level + 1U) < _levels)
                        queue.push(QueueItem(node.children[i], qi.level + 1, childrenCenters[i]));
                    else {
                        if (putMaterialIdInLeaves) node.children[i] = (id_t) triMatId;
#if 0 // outputs a debug obj of voxels as points with their colours
                        sl::color3f c;
                        _scene->getTriangleColor(iTri, c);
                        printf("v %f %f %f %f %f %f\n", childrenCenters[i][0], childrenCenters[i][1], childrenCenters[i][2], c[0], c[1], c[2]);
#endif
                    }
                }
            }
        }
    }

    cleanEmptyNodes();
    if (!internalCall) _stats.buildSVOTime = _clock.elapsed();

    // compute NVoxels
    for (id_t i = 0; i < _data[_levels - 1].size(); ++i) {
        _nVoxels += _data[_levels - 1][i].getNChildren();
    }

    _state = S_SVO;
    _stats.nNodesSVO = _nNodes;
    _stats.nNodesLastLevSVO = _data[_levels - 1].size();
    _stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
    _stats.nTotalVoxels = _nVoxels;

    if(!internalCall) printf("OK! [%s]\n",sl::human_readable_duration(_stats.buildSVOTime).c_str());

}

void GeomOctree::hiddenGeometryFloodfill() {
    printf("Starting flood fill...\n");
    if (_state != S_SVO) {
        printf("Not in SVO format, aborting!\n");
        return;
    }

    ///// Idea //////
    // From the most NXNYNZ node, start a flood fill:
    // For every one of its neighbours, if it hasn't been visited yet, check if it intersects with geometry
    // If it doesn't intersect, then continue the floodfill for its neighbours

    // Todo: This doesn't work if there is blocking geometry, e.g. a box across the whole XY axis

    ////// Helper structs/functions ////////

    // Contains all info to find a node and a reference to its parent
    struct TravNode {
        TravNode(std::shared_ptr<TravNode> prnt, id_t pIdx, ChildrenIdx i, unsigned int l)
            : parent(std::move(std::move(prnt))), parentIdx(pIdx), childIdx(i), level(l) {}
        std::shared_ptr<TravNode> parent;
        id_t parentIdx;
        ChildrenIdx childIdx;
        unsigned int level;

        bool operator<(const TravNode &other) const {
            return level < other.level
                || parentIdx < other.parentIdx
                || childIdx < other.childIdx;
        }
    };

    TravNode nullTravNode(nullptr, 0, NXNYNZ, -1);

    auto getNode = [&](const TravNode &tn) {
        if (tn.level == 0) return _data[0][0]; // Root node
//        assert((tn.level < 0 || tn.level >= _levels));
        Node& p = _data[tn.level - 1][tn.parentIdx];
        return (Node&) _data[tn.level][p.children[tn.childIdx]];
    };

    // A leaf is not represented with a node, it is a child of a node with a child bitmask of 0 or at max level (_levels)
    auto isLeaf = [&](const TravNode &tn) {
        if (tn.level == _levels) return true;
        // if tn points to an actual node, and that node has no children, then it's a leaf
        // if there is a child pointer in the parent, it's a leaf it it has no children. Else, it's a leaf
        if (getNode(*(tn.parent)).existsChildPointer(tn.childIdx)) return !getNode(tn).hasChildren();
        return true;
    };

    // Child indices with same dir (e.g. NX -> NXNYNZ, NXPYNZ, NXNYPZ, NXPYPZ)
    // Returns one of the 4 child indices in the given direction of a node (i between 0 < 4)
    auto getChildIndexForDirection = [](DirectionIdx dir, unsigned int i) {
        int     /* NX */    childIdx = i; // NX
        if      (dir % 3 == NY) childIdx = i + 2 * (i / 2);
        else if (dir % 3 == NZ) childIdx = i * 2;
        if      (dir == PX) childIdx += 4;
        else if (dir == PY) childIdx += 2;
        else if (dir == PZ) childIdx += 1;
        return childIdx;
    };


    // Neighbour finding algorithm: https://geidav.wordpress.com/2017/12/02/advanced-octrees-4-finding-neighbor-nodes/

    std::function<TravNode (const TravNode &tn, DirectionIdx dir)> getNeighbourGrtrEqSz;
    getNeighbourGrtrEqSz = [&](const TravNode &tn, const DirectionIdx dir) {
        if (tn.level == 0) return nullTravNode; // Root node, cannot go further up

        // For each of the 4 children on a side
        for (int i = 0; i < 4; ++i) {
            // If tn is a child at positive side, return a neighbour within the same parent at the opposite side, vice versa
            auto oppositeSideChildIdx = (ChildrenIdx) getChildIndexForDirection((DirectionIdx) ((dir + 3) % 6), i);
            if (tn.childIdx == oppositeSideChildIdx) {
                auto curSideChildIdx = (ChildrenIdx) getChildIndexForDirection((DirectionIdx) dir, i);
                return TravNode(tn.parent, tn.parentIdx, curSideChildIdx, tn.level);
            }
        }

        // Else, try to find the neighbour of the parent in this direction
        TravNode pNb = getNeighbourGrtrEqSz(*(tn.parent), dir);
        // Return it if it's the nullNode, or it's a leaf
        if (pNb.parent == nullptr || isLeaf(pNb)) return pNb;

        auto pNbPtr = std::make_shared<TravNode>(pNb);
        id_t pNbIdx = _data[pNb.level - 1][pNb.parentIdx].children[pNb.childIdx];
        // If the neighbour of the parent is not a leaf node, return its child that is the closest to the given node
        // For each of the 4 children on this side of the node...
        for (int i = 0; i < 4; ++i) {
            auto curSideChildIdx = (ChildrenIdx) getChildIndexForDirection((DirectionIdx) dir, i);
            if (tn.childIdx == curSideChildIdx) {
                auto oppositeSideChildIdx = (ChildrenIdx) getChildIndexForDirection((DirectionIdx) ((dir + 3) % 6), i);
                return TravNode(pNbPtr, pNbIdx, oppositeSideChildIdx, pNb.level + 1);
            }
        }

        return nullTravNode;
    };

    static std::stack<TravNode> candidates;
    auto getNeighboursSmSz = [&](const TravNode &tn, const TravNode &tnNb, const DirectionIdx dir, std::vector<TravNode> &neighbours) {
        if (tnNb.parent != nullptr) candidates.push(tnNb);

        while (!candidates.empty()) {
            const TravNode &can = candidates.top(); candidates.pop();

            if (isLeaf(can)) {
                // if it's a leaf, add as a neighbour
                neighbours.emplace_back(can);
            } else {
                // else, add children in on the opposite side of the given direction as candidates
                auto canPtr = std::make_shared<TravNode>(can);
                id_t canIdx = getNode(*(can.parent)).children[can.childIdx];
                unsigned int newLev = can.level + 1;

                // For each of the 4 children on a side of the node, add candidates
                for (int i = 0; i < 4; ++i) {
                    auto oppositeSideChildIdx = (ChildrenIdx) getChildIndexForDirection((DirectionIdx) ((dir + 3) % 6), i);
                    candidates.push(TravNode(canPtr, canIdx, oppositeSideChildIdx, newLev));
                }
            }
        }
    };

    auto getNeighbours = [&](const TravNode &tn, std::vector<TravNode> &neighbours) {
        // Get neighbours for all directions
        for (int dir = 0; dir <= PZ; ++dir) {
            // Get the neighbour directly next to it
            const TravNode &nb = getNeighbourGrtrEqSz(tn, (DirectionIdx) dir);
            // find the leaf nodes on the side of the input node inside the neighbour
            getNeighboursSmSz(tn, nb, (DirectionIdx) dir, neighbours);
        }
    };

    ///// Finding node to start the floodfill /////
    TravNode rootTrav(NULL, 0, NXNYNZ, 0);
    TravNode startTrav = nullTravNode;
    std::shared_ptr<TravNode> prevParent = std::make_shared<TravNode>(rootTrav);

    // Find the deepest node at the most negative corner of the scene
    ChildrenIdx startIdx = PXPYPZ; // Start flood fill at the top right of the scene
    id_t curNodeIdx = 0;
    for (unsigned int lev = 0; lev < _levels; ++lev) {
        Node &node = _data[lev][curNodeIdx];

        // Create new TravNode containing parent info
        TravNode curNode(prevParent, curNodeIdx, startIdx, lev + 1);
        prevParent = std::make_shared<TravNode>(curNode);
        curNodeIdx = _data[lev][curNodeIdx].children[startIdx];
        // Replace the start node with its child
        startTrav = curNode;


        // Stop if no child is to be found
        if (!node.existsChildPointer(startIdx)) {
            break;
        }
    }

    // Todo: Implementation needs some adjustments
    // In the algorithm description, empty nodes are node instances as well, as nodes with 0 children
    // In this project, empty nodes are only indicated to exist by its parent.
    // So, neighbours need to be found from nodes that don't exist, but through their description: (parent + childIdx)
    // -> TravNode can be used for this :D
    // -> almost finished this, some bugs still need fixing

    // Check if the starting node intersects with geometry: Then the initial node is inside, so not a good starting point
    if (getNode(*(startTrav.parent)).existsChild(startTrav.childIdx)) {
        printf("Deepest corner node intersects with geometry, not dealing with this now... Aborting flood fill\n");
        return;
    }

    // Flood fill, using a queue of nodes that need to be visited
    static std::vector<TravNode> neighbours;

    static std::stack<TravNode> queue;
    queue.push(startTrav);

    unsigned int numOutsideLeafs = 0;

    while (!queue.empty()) {
        const TravNode travNode = queue.top(); queue.pop();

        // For all neighbours
        neighbours.clear();
        getNeighbours(travNode, neighbours);
        for (const auto &nbTravNode : neighbours) {

            const Node &nbP = getNode(*(nbTravNode.parent)); // neighbour parent
            // Node should be checked if 1. not intersects with geometry and 2. not already marked as outside
            if (!nbP.existsChild(nbTravNode.childIdx) && !nbP.getChildOutsideBit(nbTravNode.childIdx)) {
                // Add to queue to visit next
                queue.push(nbTravNode);
                // mark as being outside of any geometry in parent
//                nbP.setChildOutsideBit(nbTravNode.childIdx);
                _data[nbTravNode.level - 1][nbTravNode.parentIdx].setChildOutsideBit(nbTravNode.childIdx);
                numOutsideLeafs++;
            }
        }
    }
    // Todo: Also consider what to happens with multiple build steps!

    // After all of the deepest nodes have been marked as inside/outside, propagate those values to their parents
    for (int lev = _levels - 2; lev >= 0; --lev) {
        int insideNodes = 0;
        for (id_t nodeId = 0; nodeId < _data[lev].size(); ++nodeId) {
            Node &parent = _data[lev][nodeId];
            for (int c = 0; c < 8; ++c) {
                if (parent.existsChildPointer(c) && _data[lev + 1][parent.children[c]].isInside()) {
                    parent.setChildOutsideBit(c);
                    insideNodes++;
                }
            }
        }
        printf("Level %i: %.2f%% of nodes inside\n", lev + 1, 100 * insideNodes / (float) _data[lev + 1].size());
    }


    printf("Done with flood fill! %u leaves outside of geometry\n", numOutsideLeafs);
}

/**
 * @brief GeomOctree::buildDAG The main function that builds sub-SVOs, individually reduces them to DAGs and merges them
 * @param levels
 * @param stepLevel
 * @param bbox
 * @param verbose
 */
void GeomOctree::buildDAG(unsigned int levels, unsigned int stepLevel, sl::aabox3d bbox, bool verbose) {
	
	printf("* Building DAG [stepLevel: %i]\n", stepLevel); fflush(stdout);
	
	std::vector< sl::point3d > leavesCenters;
	
	unsigned int stepLevels = stepLevel + 1;

	_clock.restart();
	sl::time_duration timeStamp = _clock.elapsed();

	printf("\t- Building root SVO subtree... ");
	
	buildSVO(stepLevels, bbox, true, &leavesCenters);
	hiddenGeometryFloodfill();
	
	printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str()); fflush(stdout);

	double lhs = getHalfSideD(stepLevels - 1);
	sl::vector3d corners[8];
	corners[PXPYPZ] = sl::vector3d(+lhs, +lhs, +lhs); // PXPYPZ
	corners[PXPYNZ] = sl::vector3d(+lhs, +lhs, -lhs); // PXPYNZ
	corners[PXNYPZ] = sl::vector3d(+lhs, -lhs, +lhs); // PXNYPZ
	corners[PXNYNZ] = sl::vector3d(+lhs, -lhs, -lhs); // PXNYNZ
	corners[NXPYPZ] = sl::vector3d(-lhs, +lhs, +lhs); // NXPYPZ
	corners[NXPYNZ] = sl::vector3d(-lhs, +lhs, -lhs); // NXPYNZ
	corners[NXNYPZ] = sl::vector3d(-lhs, -lhs, +lhs); // NXNYPZ
	corners[NXNYNZ] = sl::vector3d(-lhs, -lhs, -lhs); // NXNYNZ

	_stats.nNodesSVO = _nNodes;
	_stats.nNodesLastLevSVO = 0;
	std::map<unsigned int, GeomOctree *> leavesOctrees;

	float nodesProcessed = 0;
	const size_t nodesToProcess = _nVoxels;
	size_t memConsumed = 0;
	size_t acumSubtreesDAGNNodes = 0;

	printf("\t- Building %zu subtress (SVO->DAG) [%i threads]... ", _nVoxels, (int)omp_get_max_threads());
	if (verbose) printf("\n");
	timeStamp = _clock.elapsed();
	float acumDAGTimeFactor = 0;
#if 1 // More memory consumtion, but ensures CPUs occupancy
#pragma omp parallel for schedule(dynamic,2)
#else
#pragma omp parallel for schedule(static)
#endif
	for (int i = 0; i < _data[_levels - 1].size(); ++i) {
		for (int j = 7; j >= 0; --j) {
			if (_data[_levels - 1][i].existsChild(j)) {
				sl::time_point threatInitTime = _clock.now();
				GeomOctree * leafOctree = new GeomOctree(_scene);
				sl::aabox3d lbbox;
				lbbox.to_empty();
				lbbox.merge(leavesCenters[i]);
				lbbox.merge(leavesCenters[i] + corners[j]);
				leafOctree->buildSVO(levels - stepLevels, lbbox, true);
				leafOctree->hiddenGeometryFloodfill(); // todo: fix this
				size_t nNodesLeafSVO = leafOctree->getNNodes();
				size_t nNodesLeafLastLevSVO = leafOctree->_data[leafOctree->_data.size() - 1].size();
#pragma omp atomic
				_stats.nNodesSVO += nNodesLeafSVO;
#pragma omp atomic
				_stats.nNodesLastLevSVO += nNodesLeafLastLevSVO;
				size_t incVoxels = leafOctree->getNVoxels() - 1;
#pragma omp atomic
				_nVoxels += incVoxels; // root doesn't count

				leafOctree->toDAG(true);

#pragma omp atomic		
				nodesProcessed += 1.f;
				size_t incNNodes = leafOctree->getNNodes();
#pragma omp atomic		
				acumSubtreesDAGNNodes += incNNodes;
				
#pragma omp atomic
				acumDAGTimeFactor += leafOctree->getStats().toDAGTime.as_microseconds() / (float) (_clock.now()- threatInitTime).as_microseconds();
#pragma omp critical
				{
					leavesOctrees[i * 8 + j] = leafOctree;
					memConsumed += leafOctree->getMemFootprint();
					_stats.memFootprint = sl::max(_stats.memFootprint, leafOctree->getMemFootprint());
					if (verbose) {
						printf("\t\t- [%3d%%] [%3u-%u / %zu (%d)]\t%zu -> %zu\n", int(100.f * (nodesProcessed / (float)nodesToProcess)), i, j, _data[_levels - 1].size(), omp_get_thread_num(), nNodesLeafSVO, leafOctree->getNNodes());
						fflush(stdout);
					}
				}
			}
		}
	}
	acumDAGTimeFactor /= nodesProcessed;
	sl::time_duration acumDAGTime = _clock.elapsed() * acumDAGTimeFactor;

	if(verbose)
		printf("\t\t- [100%%] All subtrees done! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());
	else
		printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());

	_stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
	_stats.nTotalVoxels = _nVoxels;

	_data.resize(levels);
	_levels = levels;

	timeStamp = _clock.elapsed();

	printf("\t- Joining subtrees... ");

	for (int i = 0; i < _data[stepLevels - 1].size(); ++i) {
		for (int j = 7; j >= 0; --j) {
			if (_data[stepLevels - 1][i].existsChild(j)) {
				GeomOctree * oct = leavesOctrees[i * 8 + j];
				_data[stepLevels - 1][i].children[j] = (id_t)_data[stepLevels].size();
				for (unsigned int k = stepLevels; k < levels; ++k) {
					if (k < (levels - 1)) { // update pointers
						id_t offset = (id_t)_data[k + 1].size();
						for (unsigned int m = 0; m < oct->_data[k - stepLevels].size(); ++m) {
							for (unsigned int n = 0; n < 8; ++n) {
								if (oct->_data[k - stepLevels][m].existsChildPointer(n))
									oct->_data[k - stepLevels][m].children[n] += offset;
							}
						}
					}
					_data[k].insert(_data[k].end(), oct->_data[k - stepLevels].begin(), oct->_data[k - stepLevels].end());
				}
				delete oct;
			}
		}
	}

	printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());
	
	printf("\t- Last DAG pass to whole subtree... ");
	acumDAGTime += (_clock.elapsed() - timeStamp);
	timeStamp = _clock.elapsed();
	
	toDAG(true);

	acumDAGTime += (_clock.elapsed() - timeStamp);
	_stats.toDAGTime = acumDAGTime;
	printf("OK! [%s]\t(%zu -> %zu)\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str(), acumSubtreesDAGNNodes, _nNodes);

	_stats.buildDAGTime = _clock.elapsed();

	printf("\t- Finished! Total time [%s]\n", sl::human_readable_duration(_stats.buildDAGTime).c_str());
}

unsigned int GeomOctree::cleanEmptyNodes() {
	std::set<id_t> emptyNodes;
	unsigned int nDelPtrs = 0;
	for (int lev = _levels - 1; lev > 0; --lev) {
		emptyNodes.clear();
		for (id_t i = 0; i < _data[lev].size(); ++i) {
			if (!_data[lev][i].hasChildren()) emptyNodes.insert(i);
		}
		for (id_t i = 0; i < _data[lev-1].size(); ++i) {
			for (int j = 0; j < 8; ++j) {
				if (emptyNodes.find(_data[lev - 1][i].children[j]) != emptyNodes.end()) {
					_data[lev-1][i].unsetChildBit(j);
					_data[lev-1][i].children[j] = nullNode;
					nDelPtrs++;
				}
			}
		}
	}
	return nDelPtrs;
}


/**
 * @brief GeomOctree::toDAG Reduces a SVO to a lossy DAG
 * @param iternalCall Whether the function is called internally (only adds extra print)
 */
void GeomOctree::toLossyDAG(bool iternalCall) {

    if (_state == S_SVO) {
        if (!iternalCall) printf("* Transforming SVO -> Lossy DAG ... "); fflush(stdout);
    } else {
        printf("ERROR! This is not a SVO!\n");
        return;
    }

    /**
     * Main idea:
     * - Merge nodes that are similar (e.g. only have 1 child difference)
     * - Most effective to perform on most common nodes
     *
     * Implementation ideas
     * - Represent every child bit as a float, how close it is to the true geometry
     * -
     *
     * Proof of concept, greedy: Merge the largest sets of nodes that have diff 1
     * - Just per level for now, not earlier levels
     *
     * Looking at #nodes compressed per level w/ lossless compression, lossy compression should be avoided on:
     * - the leaves, there are very little benefits with large visual side effects
     * - the first half of levels, since this will have the largest side effects
     * -> therefore, focus on the deepest levels above the leaves
     */

    _nNodes = 1;
    /** Vector of duplicate nodes. Every index denotes the first duplicate of the node at that index in per level. */
    std::vector<id_t> correspondences;
    std::map<Node, id_t> uniqueNodesChecker;
    std::vector<Node> uniqueNodes;
    std::vector<sl::uint32_t> uniqueRefCount; // count for each uniqueNode how many refs it has

	std::vector<id_t> lossyCorrespondences; // every index indicates a node in uniqueNodes with its value as the index in lossyUniqueNodes
    std::vector<Node> lossyUniqueNodes;

    sl::time_point ts = _clock.now();

    printf("\n");

    // For every level, starting at the leaves...
    for (unsigned int lev = _levels - 1; lev > 0; --lev) {
        // Clear the lists used to keep track of correspondeces etc
        size_t oldLevSize = _data[lev].size();
        uniqueNodes.clear();
        uniqueNodes.shrink_to_fit();
        uniqueNodesChecker.clear();
        correspondences.clear();
        correspondences.resize(oldLevSize);

        uniqueRefCount.clear();
        uniqueRefCount.shrink_to_fit();
        lossyUniqueNodes.clear();
        lossyUniqueNodes.shrink_to_fit();

		// Convert to DAG as usual
        // For all nodes in this level...
        for (id_t i = 0; i < _data[lev].size(); i++) {
            Node n = _data[lev][i];
            if (!n.hasChildren()) continue; // skip empty nodes
            auto k = uniqueNodesChecker.find(n); // find if duplicate

            if (k != uniqueNodesChecker.end()) { // found
                correspondences[i] = (*k).second; // store duplicate node
                uniqueRefCount[(*k).second] += 1;
            }
            else { // !found
                uniqueNodesChecker[n] = (id_t)uniqueNodes.size(); // store it as unique node
                correspondences[i] = (id_t)uniqueNodes.size(); // the correspondence is this node itself
                uniqueNodes.push_back(n);
                uniqueRefCount.push_back(1);
            }
        }

        uniqueNodes.shrink_to_fit();
        printf("Reduced level %u \tfrom %lu  \tto %lu nodes ", lev, _data[lev].size(), uniqueNodes.size());

        // Clear original SVO nodes
        _data[lev].clear();
        _data[lev].shrink_to_fit();

        // ===== LOSSY COMPRESSION =====
        bool doLossy = lev != _levels - 1 && lev >= (_levels / 2); // only lossy on deepest 1/2 levels, except leaves

		lossyCorrespondences.clear();
		lossyCorrespondences.resize(uniqueNodes.size());

        if (doLossy) {
            // === Sort unique nodes on #references ===
            // https://stackoverflow.com/questions/26569801/sort-one-array-based-on-values-of-another-array
			std::vector<unsigned int> origOrderIndices(uniqueNodes.size());
            for (int i = 0; i < uniqueNodes.size(); ++i){
				origOrderIndices[i] = i;
            }
            std::sort(
				origOrderIndices.begin(),
				origOrderIndices.end(),
				[&uniqueRefCount](unsigned int a, unsigned int b) { return uniqueRefCount[a] > uniqueRefCount[b]; });

			// Now we have a list of indices, with those nodes with the most references at the start

            // === Check diff between each, merge those with diff 1 ===
            // Keep track of nodes that are merged
			std::map<id_t, id_t> lossyMatches;

			// Compare nodes with a low amount of references to all other nodes
			for (int i = 0; i < uniqueNodes.size(); ++i) { // nodes to potentially remove
				id_t orderedIndex = origOrderIndices[i]; // index in uniqueNodes
				if (i < uniqueNodes.size() / 2) { // skip nodes with many refs. Act like they are unique
					lossyCorrespondences[orderedIndex] = lossyUniqueNodes.size();
					lossyUniqueNodes.push_back(uniqueNodes[orderedIndex]);
					continue;
				}

				// If this node is already a match to another node, we can't remove it: skip
				if (lossyMatches.find(orderedIndex) != lossyMatches.end()) {
					// If not added yet, add it as unique node
					if (std::find(lossyUniqueNodes.begin(), lossyUniqueNodes.end(), uniqueNodes[orderedIndex]) == lossyUniqueNodes.end()) {
						lossyCorrespondences[orderedIndex] = lossyUniqueNodes.size();
						lossyUniqueNodes.push_back(uniqueNodes[orderedIndex]);
					}
					continue;
				}
				
				bool foundMatch = false;
				for (auto const& matchIndex : origOrderIndices) { // nodes to compare to, starting with node index with most refs
					// If comparing to itself, skip
					if (orderedIndex == matchIndex) continue;
					int diff = 0;
					for (int c = 0; c < 8; ++c) {
						if (lev == _levels - 1) {
							// For leaves, compare bitmask
							if (uniqueNodes[orderedIndex].existsChild(c) != uniqueNodes[matchIndex].existsChild(c)) {
								diff++;
							}
						}
						else {
							// For inner nodes, compare child pointers
							if (uniqueNodes[orderedIndex].children[c] != uniqueNodes[matchIndex].children[c]) {
								diff++;
							}
						}
					}
					// If diff is <= 1, merge!
					if (diff <= 1) {
						// since the match might not be in lossyUniqueNodes yet, don't set the corr now, but afterwards
						lossyMatches.insert(std::make_pair(orderedIndex, matchIndex)); // mark node j as a match to node i
						foundMatch = true;
						break;
					}
				}
				if (!foundMatch) { // no match, so it's unique
					lossyCorrespondences[orderedIndex] = lossyUniqueNodes.size();
					lossyUniqueNodes.push_back(uniqueNodes[orderedIndex]);
				}
			}

			// Now set the correspondences for the duplicate nodes
			for (auto const& nodeMatch : lossyMatches) {
				// The node at this original index points to an index in lossyUniqueNodes
				id_t origDupIndex = nodeMatch.first;
				id_t matchIndexInLossyUniqueNodes = std::distance(
					lossyUniqueNodes.begin(),
					std::find(
						lossyUniqueNodes.begin(),
						lossyUniqueNodes.end(),
						uniqueNodes[nodeMatch.second]
					));
				lossyCorrespondences[origDupIndex] = matchIndexInLossyUniqueNodes;
			}

            lossyUniqueNodes.shrink_to_fit();
            printf("\t-> Lossy %lu \t(%.2f%%)\n", lossyUniqueNodes.size(), 100.0 - (lossyUniqueNodes.size() / float(uniqueNodes.size())) * 100.0);
            _data[lev] = lossyUniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
        } else {
            _data[lev] = uniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
            printf("\t-> No lossy compression\n");
        }

        _data[lev].shrink_to_fit();
        _nNodes += _data[lev].size();

        // Update all pointers in the level above
        for (id_t i = 0; i < _data[lev-1].size(); i++) {
            Node * bn = &_data[lev-1][i];
            // For all children...
            for (int j = 0; j < 8; j++) {
                // If this child exists...
                if (bn->existsChild(j)) {
                    // Set the child pointer to the unique node that replaced this child
					if (doLossy) {
						bn->children[j] = lossyCorrespondences[correspondences[bn->children[j]]];
					}
					else {
						bn->children[j] = correspondences[bn->children[j]];
					}
                }
            }
        }
    }


	/**
	 * What needs to happen
	 * - Convert to DAG as usual
	 *   - This gives us a list of correspondences, which points to the new index in uniqueNodes given an index of _data[lev]
	 * - Sort uniqueNodes by ref count
	 * - Create new list of lossyUniqueNodes, of nodes that have correspondence to itself
	 * - Find matches with a low diff
	 *   - Replace list of correspondences, which points to nodes in lossyUniqueNodes given an index in _data[lev]
	 * - Update pointers in level above 
	 */

    _stats.toDAGTime = _clock.now() - ts;

    _state = S_DAG;
    _stats.nNodesDAG = _nNodes;

    if (!iternalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toDAGTime).c_str());
}

/** Recursive subtree comparison **/
bool GeomOctree::compareSubtrees(
        unsigned int levA,
        unsigned int levB,
        Node &nA, // Should be in a lower level than b
        Node &nB, // Should be in a higher level than a
        std::vector<std::map<id_t, std::pair<unsigned int, id_t>>> &nodesInSubtree
) {
	// If B terminates before A, they are not equal since one or more levels are lost
	 // Note: Subtree B is always higher up in the tree compared to subtree A (so a numerically lower level)
	if (nA.hasChildren() && !nB.hasChildren()) {
		return false;
	}
	else if (levA == _levels - 1) {
		// If nA is a leaf node, simply compare their child masks, since it doesn't matter what happens further down in B
		return nA.childrenBitmask == nB.childrenBitmask;
	}

	unsigned int childLevA = levA + 1;
	unsigned int childLevB = levB + 1;

	// If the end of the graph is reached, compare bitmasks
    if (childLevA >= _levels) {
        return nA.childrenBitmask == nB.childrenBitmask;
    }
    // Else, compare individual children: For every child
	for (int i = 0; i < 8; ++i) {

		// Todo: try out lossy compression
		// If A doesn't have a child, and B does, try to merge anyways
//        if (!)


		// If the child bits don't match, they are not equal
		if (nA.existsChild(i) != nB.existsChild(i)) {
			return false;
		}
		// Otherwise the child mask bits are equal.
		// If there is no child node, they can be seen as equal since there is no subtree below this node
		// Note: This only holds for the leaf level.

//		else if (!nA.existsChild(i) && nB.existsChild(i)) {
//			return false; // allowing this could potentially create holes in geometry
//		}
		else if (!nA.existsChild(i) && !nB.existsChild(i)) {
			continue; // if they both have no children, continue looking through other children
		}
		// Else, compare child subtrees

		// Retrieve the child nodes from their respective levels
		Node &cA = _data[childLevA][nA.children[i]];
		Node &cB = _data[childLevB][nB.children[i]];

		// Add child node to the set of unique nodes in the subtree of node A
		nodesInSubtree[childLevA][nA.children[i]] = std::make_pair(childLevB, nB.children[i]); // stores the correspondence between these two nodes

		// Now compare the subtrees of these children - only if they are not equal, we can immediately return
		if (!this->compareSubtrees(childLevA, childLevB, cA, cB, nodesInSubtree)) {
			return false;
		}
	}
	return true;
}

/** Checks whether two subtrees at different levels are equal under a specific symmetry similarity */
bool GeomOctree::compareSymSubtrees(unsigned int levA, unsigned int levB, Node &nA_in, Node &nB, bool sX, bool sY, bool sZ) {
    // Still a WIP
    // Mirror node A
    Node nA = nA_in.mirror(sX, sY, sZ);
    invertInvs(nA, levA, sX, sY, sZ);

    // Same as normal subtree comparison
    if (nA.hasChildren() && !nB.hasChildren()) {
        return false;
    } else if (!nA.hasChildren() || !nB.hasChildren()) {
        // If they both don't have children, simply compare their child masks
        return nA.childrenBitmask == nB.childrenBitmask;
    }

    unsigned int childLevA = levA + 1;
    unsigned int childLevB = levB + 1;

    // If the end of the graph is reached, they are equal (?)
    if (childLevB >= _levels || childLevA >= _levels) {
        return true;
    }

    for (int i = 0; i < 8; ++i) {
        // If they child bits don't match, they are not equal
        if (nA.existsChild(i) != nB.existsChild(i)) {
            return false;
        }
        // Otherwise they are equal.
        // If either one is not set, we cannot compare them further. Therefore they can be seen as equal
        else if (!(nA.existsChild(i) || nB.existsChild(i))) {
            continue;
        }

        Node &cA = _data[childLevA][nA.children[i]];
        Node &cB = _data[childLevB][nB.children[i]];

        // Compare their child masks...
        if (cA.childrenBitmask != cB.childrenBitmask) {
            return false;
        } else {
            // If both nodes have children...
            if (cA.hasChildren() && cB.hasChildren()) {

                // Then compare the subtrees of these children
                if (!this->compareSymSubtrees(childLevA, childLevB, cA, cB, sX, sY, sZ)) {
                    return false;
                }
            }
            // If only 1 node has children, they can be seen as equal. One has more detail than the other
        }
    }
    return true;
}

void GeomOctree::initChildLevels() {
    // Correcting child levels, in case multiple build steps were used
    for (unsigned int lev = 0; lev < _levels; ++lev) {
        for (id_t nodeIndex = 0; nodeIndex < _data[lev].size(); ++nodeIndex) {
            for (unsigned int c = 0; c < 8; ++c) {
                _data[lev][nodeIndex].childLevels[c] = lev + 1;
            }
        }
    }
}

void GeomOctree::symMergeAcrossAllLevels() {
    // Needs to be in SDAG state

    if (_state == S_SDAG) {
        printf("* Transforming SDAG -> CSDAG ... "); fflush(stdout);
    } else {
        printf("ERROR! This is not a SDAG!\n");
        return;
    }

    // Build multi-map of MirrorNodes
    struct MirroredNode{
        MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
        MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
                mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
        bool mirrorX, mirrorY, mirrorZ;
        id_t id;
    };

    // For each of the 8 symmetry options, for each level, a multi-map of hash -> index for each node
    // None, X, Y, Z, XY, XZ, YZ, XYZ
    std::vector<std::vector<std::multimap<uint64_t, id_t, uint8_t>>> matchMaps(9);
    for (int i = 0; i < 8; ++i) {
        matchMaps[i].resize(_levels);
    }

    auto buildMultiMap = [&](unsigned int depth) {
        printf("Building match maps at depth %u... ", depth);
        fflush(stdout);
        for (unsigned int lev = 0; lev < _levels - depth; ++lev) {
            matchMaps[lev].clear();
            for (id_t nodeIndex = 0; nodeIndex < _data[lev].size(); ++nodeIndex) {

                // Todo: Insert all symmetry options in matchMaps
                for (int symIndex = 0; symIndex < 8; ++symIndex) {
                    // Todo: Check invariance
                    uint64_t key = 0; // computeNodeKey(_data[lev][nodeIndex], depth);
//                    matchMaps[symIndex][lev].insert(std::make_pair(key, nodeIndex));
                }
            }
        }
        printf("Done!\n");
    };


    unsigned int currentMatchDepth = _levels / 2;

    // Store correspondences across different levels: For every level, a map of a node and its match (level, index, symmetry index)
    std::vector<std::map<id_t, std::pair<unsigned int, std::pair<id_t, int>>>> multiLevelCorrespondences(_levels);
    // Store all nodes that are visited in a subtree, so they can be removed when a correspondence is found
    std::vector<std::map<id_t, std::pair<unsigned int, id_t>>> nodesInCurSubtree(_levels);

    // Todo: Use structs instead of pairs in pairs in pairs

    //
    unsigned int levA = 1;
    for (; levA < _levels; ++levA) {

        unsigned int maxMatchDepth = _levels - levA - 1;
        if (maxMatchDepth < currentMatchDepth) {
            currentMatchDepth = maxMatchDepth;
            buildMultiMap(currentMatchDepth);
        }

        for (unsigned int idA = 0; idA < _data[levA].size(); ++idA) {
            Node &nA = _data[levA][idA];
            // Todo: Check if already matched through a parent node

            bool foundMatch = false;

            uint64_t nAKey = 0; // computeNodeKey(nA, currentMatchDepth);
            for (unsigned int levB = 0; levB < levA; ++levB) {
                for (int symIndex = 0; symIndex < 8; ++symIndex) {
//                    auto matchResult = matchMaps[symIndex][levB].equal_range(nAKey);
//                    for (auto it = matchResult.first; it != matchResult.second; ++it) {
//                        id_t idB = it->second;
//                        bool areSubtreesEqual = false; // compareSymSubtrees(levA, levB, nA, nB, nodesInCurSubtree);
//                        if (areSubtreesEqual) {
//                            foundMatch = true;
//                            multiLevelCorrespondences[levA][idA] = std::make_pair(levB, std::make_pair(idB, symIndex));
//                            // Append all nodes in this subtree to the nodes that can be removed
//                            for (int levRem = 0; levRem < _levels; ++levRem) { // levels below the root
//                                // Todo: take into account mirror bits of child pointers
////                                multiLevelCorrespondences[levRem].insert(nodesInCurSubtree[levRem].begin(), nodesInCurSubtree[levRem].end());
//                            }
//                            break;
//                        }
//                    }
//                    if (foundMatch) break;
                }
                if (foundMatch) break;
            }
        }
    }


}

/**
 * @brief mergeAcrossAllLevels Brute force search over all levels, looking for equal subtrees and merging them
 * Goal: Research to see what the benefit of multi-level merging would be.
 * Just for SVDAGs now, but SSVDAGs would likely perform better - harder to check though
 */
unsigned int GeomOctree::mergeAcrossAllLevels() {
    /** Computes a uint64_t key based on the child bitmasks of a node's children **/
    std::function<uint64_t (const GeomOctree::Node &node, unsigned int depth)> computeNodeKey;
    std::hash <uint64_t> hash64;
    computeNodeKey = [&](const GeomOctree::Node &node, unsigned int depth) {
        uint64_t key = 0;

        // At depth 0, just return the child mask
        if (depth == 0) {
            key += node.childrenBitmask;
            return key;
        }
            // At depth 1, concat all 8-bit child masks into a 64 bit int
        else if (depth == 1) {
            for (unsigned int c = 0; c < 8; ++c) {
                // shift the bit mask of children into the key
                key = key << 8u;
                if (node.existsChildPointer(c)) {
                    key += _data[node.childLevels[c]][node.children[c]].childrenBitmask;
                }
            }
        }
            // For higher depths, combine all children keys with XOR
        else {
            for (unsigned int c = 0; c < 8; ++c) {
                if (node.existsChildPointer(c)) {
                    const auto &child = _data[node.childLevels[c]][node.children[c]];
                    uint64_t childKey = computeNodeKey(child, depth - 1);

                    key = key << 1u; // bit shift so that identical child hashes don't cancel each other out
                    key = hash64(key) ^ hash64(childKey); // xor of hash of current key and child key
                }
            }
        }
        return key;
    };


    std::function<std::string (const GeomOctree::Node &node, unsigned int depth)> computeNodeString;
    computeNodeString = [&](const GeomOctree::Node &node, unsigned int depth) {
        std::string key = "";
        // At depth 0, just return the child mask
        if (depth == 0) {
            key += std::to_string(node.childrenBitmask);
            return key;
        }
            // At depth 1, concat all 8-bit child masks into a 64 bit int
        else if (depth == 1) {
            for (unsigned int c = 0; c < 8; ++c) {
                // shift the bit mask of children into the key

                if (node.existsChildPointer(c)) {
                    key += std::to_string(_data[node.childLevels[c]][node.children[c]].childrenBitmask);
                } else {
                    key += "x";
                }
            }
        }
            // For higher depths, combine all children keys with XOR
        else {
            for (unsigned int c = 0; c < 8; ++c) {
                if (node.existsChildPointer(c)) {
                    const auto &child = _data[node.childLevels[c]][node.children[c]];
                    std::string childKey = computeNodeString(child, depth - 1);
                    key += "-" + childKey;
                } else {
                    key += "-x";
                }
            }
        }
        return key;
    };

    // Prints all childmasks of two subtrees
	std::function<int (unsigned int levA, unsigned int levB,
		const GeomOctree::Node &nA, const GeomOctree::Node &nB)> getMasks;
	getMasks = [&](unsigned int levA, unsigned int levB, const GeomOctree::Node &nA, const GeomOctree::Node &nB) {
		printf("%u %u\n", nA.childrenBitmask, nB.childrenBitmask);
		if (levA == _levels - 1) {
			return 0;
		}
		for (int c = 0; c < 8; ++c) {
			printf("C%u: ", c);
			if (nA.existsChild(c)) {
				getMasks(levA + 1, levB + 1, _data[levA + 1][nA.children[c]], _data[levB + 1][nB.children[c]]);
			} else if (nB.existsChild(c)) {
				printf("X %u !!! !!! !!!\n", _data[levB + 1][nB.children[c]].childrenBitmask);
			} else {
				printf("X X\n");
			}
		}
		return 0;
	};



    ///////////////////////////////////////////////////////////////
    /// Building multi-maps for finding potential matches faster //
    ///////////////////////////////////////////////////////////////
	// try out unordered map for performance improvements --->>> nothing changed
    std::vector<std::multimap<uint64_t, id_t>> matchMaps(_levels);

    auto buildMultiMap = [&](unsigned int depth, bool writeToFile = false) {
        printf("Building match maps at depth %u... ", depth);
		fflush(stdout);

		std::ofstream myfile(
			"C:\\Users\\Remi\\Documents\\Projects\\Thesis\\CRS4-SSVODAGs\\bin\\keys\\keys-" + std::to_string(depth) + ".txt",
			std::ios::out | std::ios::trunc);
        for (unsigned int lev = 0; lev < _levels - depth; ++lev) {
            matchMaps[lev].clear();
            for (id_t nodeIndex = 0; nodeIndex < _data[lev].size(); ++nodeIndex) {
                uint64_t key = computeNodeKey(_data[lev][nodeIndex], depth);
                matchMaps[lev].insert(std::make_pair(key, nodeIndex));

				if (writeToFile)
					myfile << std::to_string(lev) + ", " + std::to_string(_data[lev][nodeIndex].childrenBitmask) + " ->\t " + std::to_string(key) + "\n";
            }
        }

		myfile.close();

		printf("Done!\n");
    };

    unsigned int currentMatchDepth = _levels / 2;
    // Todo: Initial depth should depend on total # of levels, 1 seems enough for ~8K, 2 or higher for more
    buildMultiMap(currentMatchDepth);

    /////////////////////////////////
    /// Finding identical subtrees //
    /////////////////////////////////

    // Comparing ALL combinations of subtree pairs is not what we want
    // If a subtree high-up is equal to another subtree, all of its sub-subtrees will equal it as well
    // So, only keep track of children that are NOT already equal to another subtree
    // Only compare those instead of all nodes in the next level
    // Main algorithm:
    // For every node to currently check...
        // Check for levels higher-up whether there is an identical subtree
        // Note: If node B terminates before node A, they are NOT equal. This will not happen since we only compare to subtrees higher-up.
        // For nodes that are not equal, add their children in the next level to the nextNodesToCheck list.
    std::set<id_t> currentNodesToCheck;
    std::set<id_t> nextNodesToCheck;

    // Initialize all nodes of the highest level - 1 for comparison to subtrees at higher levels
    // This way, the matches are found as early as possible
    unsigned int levA = 1;
    for (id_t i = 0; i < _data[levA].size(); i++) {
        currentNodesToCheck.insert(i);
    }

    // Some variables for analytics
    unsigned int numTotalComparisons = 0;
    size_t prevNNodes = _nNodes;
    _nNodes = 1;

    // We need to store the correspondences of one subtree to another:
    // Contains for each level, a map of node IDs that point to level and index of an identical subtree higher-up in the graph.
    std::vector<std::map<id_t, std::pair<unsigned int, id_t>>> multiLevelCorrespondences(_levels); // store correspondences across different levels

    // Store all nodes that are visited in a subtree, so they can be potentially removed if a correspondence is found
    std::vector<std::map<id_t, std::pair<unsigned int, id_t>>> nodesInCurSubtree(_levels);

    // The level of each child pointer is set in node.childLevels[k]
    // then at the end, replace the node data in each level in those subtrees

    // The larger subtree is always found first, since duplicate subtrees are found top-down

    // Algorithm for removing duplicates so that indices don't get messed up:
    // - Idea: Same way as toDAG: Keep track of correspondences, replace data of whole level at one time bottom-up

    printf("Starting multi-level subtree comparisons:\n");

    int totHashCollisions = 0;

    for (; levA < _levels; ++levA) {
        _clock.restart();
        printf(" - L%u (%zu / %zu to check)... \t", levA, currentNodesToCheck.size(), _data[levA].size());
        fflush(stdout);

        // Build new match maps for the lowest levels with lower depths, when those nodes do not have subtrees of that depth
        unsigned int maxMatchDepth = _levels - levA - 1;
        if (maxMatchDepth < currentMatchDepth) {
			currentMatchDepth = maxMatchDepth; // currentMatchDepth / 2;
            buildMultiMap(currentMatchDepth);
        }

		int stepLogger = (int)round((currentNodesToCheck.size() + 1) / 10.f);

        unsigned long totalMatchCount = 0;

        // For all nodes to be checked...
		unsigned int curNodeIndex = 0;
		for (const auto& idA : currentNodesToCheck) {
            if ((curNodeIndex % stepLogger == 0)) {
                printf("%.0f%%..", round(100.f * (curNodeIndex / (float)currentNodesToCheck.size())));
                fflush(stdout);
            }
			curNodeIndex++;

            Node &nA = _data[levA][idA];
            uint64_t nAKey = computeNodeKey(nA, currentMatchDepth);

            bool foundMatch = false;

            int hashCollisions = 0;

            // also loop over levA itself, to find matches in the same level? No, that is already done in DAG
            for (unsigned int levB = 0; levB < levA; ++levB) {
                // idea: instead of checking in 'chronological' order, start looking at commonly chosen subtrees first
                // would probably not be worth it, since only a small portion of all nodes gets matches. Current optimization (hash) seems good enough

                // Instead of looping over EVERY node, just loop over potential matches!
                auto matchResult = matchMaps[levB].equal_range(nAKey);
                totalMatchCount += matchMaps[levB].count(nAKey);
                for (auto it = matchResult.first; it != matchResult.second; ++it) {
                    id_t j = it->second;
                    numTotalComparisons++;
                    Node &nB = _data[levB][j];

                    for (int i = 0; i < _levels; ++i) {
                        nodesInCurSubtree[i].clear();
                    }

                    // Brute force check equality of subtrees of nA and nB
                    bool areSubtreesEqual = compareSubtrees(levA, levB, nA, nB, nodesInCurSubtree);

                    if (areSubtreesEqual) {
						// printf("\nMatch! LA %u LB %u, IDA: %u IDB %u\n", levA, levB, idA, j);
						// getMasks(levA, levB, nA, nB);
                        foundMatch = true;
                        // Store that the subtree under the root of nodeA is identical to the subtree under nodeB
                        multiLevelCorrespondences[levA][idA] = std::make_pair(levB, j);

                        // Append all nodes in this subtree to the nodes that can be removed
                        for (int levRem = 0; levRem < _levels; ++levRem) { // levels below the root
                            multiLevelCorrespondences[levRem].insert(nodesInCurSubtree[levRem].begin(), nodesInCurSubtree[levRem].end());
                        }
                        //printf("-OK MATCH-");
                        break;
                    } else if (maxMatchDepth == currentMatchDepth) {
//                        std::cout << std::endl << computeNodeString(nA, currentMatchDepth) << std::endl;
//                        std::cout << computeNodeString(nB, currentMatchDepth) << std::endl;
////                        getMasks(levA, levB, nA, nB);
//                        printf("Matches correspond at same depth level, but no equal?!?!\n");
                        hashCollisions++;
                    }
                }
                if (foundMatch) {
                    break; // If a match is found, we are done for this subtree. It is equal to a subtree higher up in the graph!
                }
            }
            if (hashCollisions > 0) {
//                printf("Hash collisions: %i\n", hashCollisions);
                totHashCollisions += hashCollisions;
            }
            if (!foundMatch) {
                // If no match was found, try to find duplicate subtrees for all child nodes in the next iteration
                // Todo: This makes it brute force, is kinda expensive. Is there a more efficient way do fewer comparisons?
                for (int i = 0; i < 8; ++i) {
                    if (nA.existsChild(i)) {
                        nextNodesToCheck.insert(nA.children[i]);
                    }
                }
            }
        }

        printf("Avg matches found per node: %.0f\n", totalMatchCount / float(currentNodesToCheck.size()));

        auto time = _clock.elapsed();

        printf(" -> %lu (%.0f%%) [%s]\n", _data[levA].size() - multiLevelCorrespondences[levA].size(), 100 * multiLevelCorrespondences[levA].size() / (float) _data[levA].size(), sl::human_readable_duration(time).c_str());

        // After all nodes for this level have been checked, swap the current and next nodes to check
        currentNodesToCheck.clear();
        currentNodesToCheck.swap(nextNodesToCheck);
    }


    printf("Total hash collisions: %i\n", totHashCollisions);

    ///////////////////////////////////
    /// Removing identical subtrees ///
    ///////////////////////////////////

    // Now that all identical subtrees have been identified, the duplicate subtrees can be removed and the pointers to them should be updated.
    for (unsigned int lev = _levels - 1; lev > 0; --lev) {
        std::vector<Node> uniqueNodes;
        uniqueNodes.reserve(_data[lev].size() - multiLevelCorrespondences[lev].size());

        std::vector<id_t> correspondences(_data[lev].size(), -1); // normal correspondences for this level (old index -> new index) for those that are not removed

        for (id_t nodeIndex = 0; nodeIndex < _data[lev].size(); ++nodeIndex) {
            // insert all nodes in uniqueNodes that do not have a correspondence in a higher level
            if (multiLevelCorrespondences[lev].count(nodeIndex) == 0) {
                correspondences[nodeIndex] = uniqueNodes.size();
                uniqueNodes.push_back(_data[lev][nodeIndex]);
            }
        }

        // Replace node data for this level
        _data[lev].clear();
        _data[lev].shrink_to_fit();
        _data[lev] = uniqueNodes;
        _data[lev].shrink_to_fit();

        _nNodes += _data[lev].size();

        int numReplaced = 0;
        int numRemained = 0;

        // Update all pointers in the level above
		for (id_t nodeIndex = 0; nodeIndex < _data[lev-1].size(); ++nodeIndex) {
			Node *node = &_data[lev - 1][nodeIndex];
			// For all children...
			for (int j = 0; j < 8; j++) {
				// If this child exists...
				if (node->existsChild(j)) {
					// If it was replaced by a subtree higher up
					auto it = multiLevelCorrespondences[lev].find(node->children[j]);
					if (it != multiLevelCorrespondences[lev].end()) {
						node->childLevels[j] = it->second.first;
						node->children[j] = it->second.second;
						// Node order in a higher level will change in future iteration...
						// Therefore, the next loop updates pointers of nodes in lower level that point to nodes in this level
						numReplaced++;
					} else {
						// Else, update the index from the normal list of correspondences
						node->children[j] = correspondences[node->children[j]];
						numRemained++;
					}
				}
			}
		}
		

        // Update pointers from lower levels to nodes in this level
        for (unsigned int levLow = _levels - 2; levLow >= lev; --levLow) {
            for (id_t nodeIndex = 0; nodeIndex < _data[levLow].size(); ++nodeIndex) {
                Node *node = &_data[levLow][nodeIndex];
                // For all children...
                for (int j = 0; j < 8; j++) {
                    // If this node from a lower level points a node in the current level...
                    if (node->existsChild(j) && node->childLevels[j] == lev) {
                        if (correspondences[node->children[j]] == (id_t) -1) {
                            printf("\t\t- Missing correspondence on lev %u: Node %u, child %u\n", levLow, nodeIndex, j);
                        }

                        // Update where that pointer has been moved to
                        node->children[j] = correspondences[node->children[j]];
                    }
                }
            }
        }

        printf(" - L %i Replaced/remained: %i / %i\n", lev, numReplaced, numRemained);
    }

    _stats.nNodesDAG = _nNodes;


    /////////////////////////////////
    /// Done: Print the results!   //
    /////////////////////////////////
    printf("Total #nodes %zu / %zu. Total #comparisons: %u\n", _nNodes, prevNNodes, numTotalComparisons);

    int totalElimNodes = 0;
    for (unsigned int i = 0; i < _levels; ++i) {
        totalElimNodes += multiLevelCorrespondences[i].size();
        id_t origSize = _data[i].size() + multiLevelCorrespondences[i].size();
        double pct = 100 * (multiLevelCorrespondences[i].size() / double(origSize));
        printf(" - Level %u:   \t%zu subtrees are equal to a subtree higher up. %zu / %i (%.2f%%) nodes of this level have been removed\n", i, multiLevelCorrespondences[i].size(), multiLevelCorrespondences[i].size(), origSize, pct);
    }
    printf("Total number of nodes that was removed: %u / %zu (%.2f%%)\n ", totalElimNodes, prevNNodes, (100 * totalElimNodes / double(prevNNodes)));


//    printf("Indirect subtree feasibility: How many unique pointers there are to other levels, per level\n");
//    for (unsigned int lev = 0; lev < _levels; ++lev) {
//        std::vector<std::set<id_t>> uniquePointers(_levels);
//        std::vector<unsigned int> numPointers(_levels);
//        for (const auto &node : _data[lev]) {
//            for (int c = 0; c < 8; ++c) {
//                if (node.childLevels[c] != lev + 1) {
//                    uniquePointers[node.childLevels[c]].insert(node.children[c]);
//                    numPointers[node.childLevels[c]]++;
//                }
//            }
//        }
//
//        unsigned int totalNumUniquePointers = 0;
//        unsigned int totalNumPointers = 0;
//        for (unsigned int lev2 = 0; lev2 <= lev; ++lev2) {
//            printf("    L%u -> L%u: Unique / total = %zu / %u\n", lev, lev2, uniquePointers[lev2].size(), numPointers[lev2]);
//            totalNumUniquePointers += uniquePointers[lev2].size();
//            totalNumPointers += numPointers[lev2];
//        }
//        printf("  L%u total: Unique / total = %u / %u\n", lev, totalNumUniquePointers, totalNumPointers);
//    }


    return totalElimNodes;
}

/**
 * @brief GeomOctree::findAllSymDuplicateSubtrees Same as findAllDuplicateSubtrees, but with
 * symmetry as well: Find symmetricly identical subtrees over all levels
 * @return
 */
unsigned int GeomOctree::findAllSymDuplicateSubtrees() {
     // Still a WIP

    struct MirroredNode{
        MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
        MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
            mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
        bool mirrorX, mirrorY, mirrorZ;
        id_t id;
    };

    // Note: Comparing ALL combinations of subtree pairs is not what we want
    // If a subtree high-up is equal to another subtree, all of its subtrees will equal it as well

    // Solution(?): Keep track of children that are NOT equal to another subtree
    // Only compare those instead of all nodes in the next level
    std::vector<Node> currentNodesToCheck;
    std::vector<Node> nextNodesToCheck;

    // Initialize all nodes of the highest level for comparison to subtrees at other levels
    unsigned int levA = 1;
    for (id_t i = 0; i < _data[levA].size(); i++) {
        Node &n = _data[levA][i];
        currentNodesToCheck.push_back(n);
    }

    unsigned int numEqualSubtrees = 0;
    unsigned int numTotalComparisons = 0;

    // Most efficient methinks:
    // Start at level 1 (A), compare to all nodes at levels above it (B), repeat for following levels
    // This way, the matches are found as early as possible

    // For every node to check...
        // Check for any level if there is an equal subtree
        // Note: If node B terminates before node A, they are NOT equal. The other way around is fine
        // For nodes that are not equal, add their children in the next level to the nextNodesToCheck list!

    std::vector<int> nodesEqualPerLevel(_levels, 0);

    for (; levA < _levels; ++levA) {
        printf("Comparing level %u\n", levA);
        // For all nodes to be checked...
        for (auto& nA : currentNodesToCheck) {

            bool foundMatch = false;
            for (unsigned int levB = 0; levB < levA; ++levB) {
                // For all nodes in the other level above A...
                for (id_t j = 0; j < _data[levB].size(); j++) {
                    numTotalComparisons++;

                    Node &nB = _data[levB][j];

                    // TODO: Compare all mirrored nodes of subtree nA to original nodes of subtree nB
                    // * Compute mirrored subtrees of nA
                    // * Compare each of them as before with nB
                    // * Repeat!

                    unsigned int nodesInSubtree = 1;

                    // Brute force check equality of subtrees of nA and nB
                    if (compareSymSubtrees(levA, levB, nA, nB, false, false, false)
                        || compareSymSubtrees(levA, levB, nA, nB, true, false, false)
                        || compareSymSubtrees(levA, levB, nA, nB, false, true, false)
                        || compareSymSubtrees(levA, levB, nA, nB, false, false, true)
                        || compareSymSubtrees(levA, levB, nA, nB, true, true, false)
                        || compareSymSubtrees(levA, levB, nA, nB, true, false, true)
                        || compareSymSubtrees(levA, levB, nA, nB, false, true, true)
                        || compareSymSubtrees(levA, levB, nA, nB, true, true, true)
                    ) {
                        foundMatch = true;
                        numEqualSubtrees++;
                        nodesEqualPerLevel[levA]++;
                        break;
                    }
                }
                if (foundMatch) {
                    // If a match is found, we are done for this subtree
                    // It is equal to a subtree higher up in the graph!
                    break;
                }
            }
            if (!foundMatch) {
                // If no match was found, try later for all child nodes
                for (int i = 0; i < 8; ++i) {
                    if (nA.existsChild(i)) {
                        nextNodesToCheck.push_back(_data[levA + 1][nA.children[i]]);
                    }
                }
            }
        }

        // Since children of a node may also be children of other nodes in a DAG,
        // we need to ensure children are only present once to the nextNodesToCheck vector
        std::sort(nextNodesToCheck.begin(), nextNodesToCheck.end() );
        nextNodesToCheck.erase(std::unique(nextNodesToCheck.begin(), nextNodesToCheck.end() ), nextNodesToCheck.end());

        // After all nodes for this level have been checked, swap the current and next nodes to check
        currentNodesToCheck.clear();
        currentNodesToCheck.swap(nextNodesToCheck);
    }

    printf("Nodes equal to another one: %u. Total #nodes %zu. Total #comparisons: %u\n", numEqualSubtrees, _nNodes, numTotalComparisons);

    for (unsigned int i = 0; i < _levels; ++i) {
        double pct = 100 * (nodesEqualPerLevel[i] / double(_data[i].size()));
        printf("- Level %u:   \t%i / %zu (%.2f%%) subtrees are equal to a subtree higher up\n", i, nodesEqualPerLevel[i], _data[i].size(), pct);
    }
    printf("Total equal: %u / %zu (%.2f%%)\n", numEqualSubtrees, _nNodes, (100 * numEqualSubtrees / double(_nNodes)));

    return numEqualSubtrees;
}


/**
 * @brief GeomOctree::toDAG Reduces an SVO to a DAG
 * @param iternalCall Whether the function is called internally (only adds extra print)
 */
void GeomOctree::toDAG(bool iternalCall) {

	if (_state == S_SVO) {
		if (!iternalCall) printf("* Transforming SVO -> DAG ... "); fflush(stdout);
	} else {
		printf("ERROR! This is not a SVO!\n");
		return;
	}

	_nNodes = 1;
    /** Every index denotes the index of the first duplicate of that node in uniqueNodes. Reset for each level. */
	std::vector<id_t> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	 sl::time_point ts = _clock.now();

	 if (!iternalCall)
        printf("\n");

    // For every level, starting at the leaves...
	for (unsigned int lev = _levels - 1; lev > 0; --lev) {
        // Clear the lists used to keep track of correspondences etc
		size_t oldLevSize = _data[lev].size();
		uniqueNodes.clear();
		uniqueNodes.shrink_to_fit();
		uniqueNodesChecker.clear();
		correspondences.clear();
		correspondences.resize(oldLevSize);

        // For all nodes in this level...
		for (id_t i = 0; i < _data[lev].size(); i++) {
			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes
            auto k = uniqueNodesChecker.find(n); // find if duplicate

			if (k != uniqueNodesChecker.end()) { // found
                correspondences[i] = (*k).second; // store duplicate node
			}
			else { // !found
                uniqueNodesChecker[n] = (id_t)uniqueNodes.size(); // store it as unique node
                correspondences[i] = (id_t)uniqueNodes.size(); // the correspondence is this node itself
				uniqueNodes.push_back(n);
			}
		}

		if (!iternalCall)
            printf("Reduced level %u from %lu to %lu nodes\n", lev, _data[lev].size(), uniqueNodes.size());

		_data[lev].clear();
		_data[lev].shrink_to_fit();
		uniqueNodes.shrink_to_fit();
        _data[lev] = uniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();
		
        // Update all pointers in the level above
		for (id_t i = 0; i < _data[lev-1].size(); i++) {
			Node * bn = &_data[lev-1][i];
            // For all children...
			for (int j = 0; j < 8; j++) {
                // If this child exists...
                if (bn->existsChild(j)) {
                    // Set the child pointer to the unique node that replaced this child
                    bn->children[j] = correspondences[bn->children[j]];

//                    if (lev >= _levels - 1) {
//
//                        // test to see if recursion works. post-test: YES IT DOES. Need to set LEVELS uniform to *2 though
//                        if ((std::rand() % 100) / 100.0 > 0.5) {
//                            bn->children[j] = 0;
//                            bn->childLevels[j] = 1;
//                        }
//                    }
                }
			}
		}
	}

	_stats.toDAGTime = _clock.now() - ts;
	
	_state = S_DAG;
	_stats.nNodesDAG = _nNodes;

	if (!iternalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toDAGTime).c_str());
}


void GeomOctree::toSDAG(bool internalCall, bool skipSymmetry) {

	if (!internalCall) {
		if (_state == S_SVO) {
			printf("* Transforming SVO -> SDAG (not best way)... "); fflush(stdout);
		}
		else if (_state == S_DAG) {
			printf("* Transforming DAG -> SDAG (normal)... "); fflush(stdout);
		}
		else {
			printf("ERROR! This is not a DAG or SDAG!\n");
			return;
		}
	}

	struct MirroredNode{
		MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
		MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
			mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
		bool mirrorX, mirrorY, mirrorZ;
		id_t id;
	};

	std::vector<MirroredNode> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	_nNodes = 0;

	if (!internalCall) _clock.restart();

    // For every level, starting at the leaves...
	for (unsigned int lev = _levels - 1; lev > 0; lev--) {
        // Clear the lists used to keep track of correspondeces etc
		unsigned int oldLevSize = (unsigned int)_data[lev].size();
		uniqueNodes.clear();
		uniqueNodesChecker.clear();
		correspondences.resize(oldLevSize);

        // For each node in this level...
		for (id_t i = 0; i < _data[lev].size(); i++) {

			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes

			if (skipSymmetry) { // Used when we only want pointer compression
                uniqueNodesChecker[n] = (id_t)uniqueNodes.size();
                correspondences[i] = MirroredNode(false, false, false, id_t(uniqueNodes.size()));
                uniqueNodes.push_back(n);
                continue;
			}

            // Check invariances
            if (n == n.mirror(true, false, false)) n.setInvariantBit(0);
            if (n == n.mirror(false, true, false)) n.setInvariantBit(1);
            if (n == n.mirror(false, false, true)) n.setInvariantBit(2);

            // Create mirrored versions of this node
            Node niX = n.mirror(true, false, false);
            Node niY = n.mirror(false, true, false);
            Node niZ = n.mirror(false, false, true);
            Node niXY = niX.mirror(false, true, false);
            Node niXZ = niX.mirror(false, false, true);
            Node niYZ = niY.mirror(false, false, true);
            Node niXYZ = niXY.mirror(false, false, true);

            // If a child is invariant on an axis, this removes the mirror bit there
			if (lev < (_levels - 1)) {
				invertInvs(niX, lev, true, false, false);
				invertInvs(niY, lev, false, true, false);
				invertInvs(niZ, lev, false, false, true);
				invertInvs(niXY, lev, true, true, false);
				invertInvs(niXZ, lev, true, false, true);
				invertInvs(niYZ, lev, false, true, true);
				invertInvs(niXYZ, lev, true, true, true);
			}

            // Find a correspondence of this node or any of its mirrored copies
			std::map<GeomOctree::Node, sl::uint32_t>::iterator it;
			if ((it = uniqueNodesChecker.find(n)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, false, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niX)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, false, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niY)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, true, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, false, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXY)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, true, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, false, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niYZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, true, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXYZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, true, true, (*it).second);
			}
            else { // no correspondence found, so add as unique node
				uniqueNodesChecker[n] = (id_t)uniqueNodes.size();
				correspondences[i] = MirroredNode(false, false, false, id_t(uniqueNodes.size()));
				uniqueNodes.push_back(n);
			}
		}
        // Replace previous nodes on this level as the reduced set of symmetrically unique nodes
		_data[lev].clear();
		_data[lev] = uniqueNodes;
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();

        // Update all pointers in the level above
		for (id_t i = 0; i < _data[lev - 1].size(); i++) {
			Node &n = _data[lev - 1][i];
			for (id_t j = 0; j < 8; j++) {
				if (n.existsChildPointer(j)) {
                    // Set the child pointer to the unique node that replaced this child
					MirroredNode mn = correspondences[n.children[j]];
					n.children[j] = mn.id;
					if (mn.mirrorX) n.setChildMirrrorBit(0, j);
					if (mn.mirrorY) n.setChildMirrrorBit(1, j);
					if (mn.mirrorZ) n.setChildMirrrorBit(2, j);
				}
			}
		}
	}

	if (!internalCall) _stats.toSDAGTime = _clock.elapsed();
	_stats.nNodesSDAG = _nNodes;
	_state = S_SDAG;
	
#if 0 // DEGUG HOW MUCH MIRRORED NODES THERE ARE
	for (int lev = 0; lev < _maxLevel; ++lev) {
		int sCount = 0;
		for (int i = 0; i < _svoLevelsHDD[lev]->size(); ++i) {
			BuildNode bn = _svoLevelsHDD[lev]->item(i);
			if (bn.mirroredX != 0 || bn.mirroredY != 0 || bn.mirroredZ != 0) sCount++;
		}
		printf("LEVEL %d :\t %d nodes with some symm flag\n", lev, sCount);
	}
#endif
	if(!internalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toSDAGTime).c_str());
}


void GeomOctree::toSDAGCanonical() {

	printf("* Transforming DAG -> SDAG (canonical)... "); fflush(stdout);

	struct MirroredNode {
		MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
		MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
			mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
		bool mirrorX, mirrorY, mirrorZ;
		id_t id;
	};

	std::vector<MirroredNode> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	_nNodes = 0;

	_clock.restart();

	for (unsigned int lev = _levels - 1; lev > 0; lev--) {
		unsigned int oldLevSize = (unsigned int)_data[lev].size();
		uniqueNodes.clear();
		uniqueNodesChecker.clear();
		correspondences.resize(oldLevSize);

		for (id_t i = 0; i < _data[lev].size(); i++) {

			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes

			// Set invariances
			if (n == n.mirror(true, false, false)) n.setInvariantBit(0);
			if (n == n.mirror(false, true, false)) n.setInvariantBit(1);
			if (n == n.mirror(false, false, true)) n.setInvariantBit(2);

			bool cx, cy, cz;
			Node nC = n.getCanonical(cx, cy, cz);

			// Check children invariances
			if (lev < (_levels - 1)) invertInvs(nC, lev, cx, cy, cz);

			if (uniqueNodesChecker.find(nC) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(cx, cy, cz, uniqueNodesChecker[nC]);
			} else { // !found
				uniqueNodesChecker[nC] = (id_t)uniqueNodes.size();
				correspondences[i] = MirroredNode(cx, cy, cz, id_t(uniqueNodes.size()));
				uniqueNodes.push_back(nC);
			}
		}
		_data[lev].clear();
		_data[lev] = uniqueNodes;
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();

		for (id_t i = 0; i < _data[lev - 1].size(); i++) {
			Node &n = _data[lev - 1][i];
			for (id_t j = 0; j < 8; j++) {
				if (n.existsChildPointer(j)) {
					MirroredNode mn = correspondences[n.children[j]];
					n.children[j] = mn.id;
					if (mn.mirrorX) n.setChildMirrrorBit(0, j);
					if (mn.mirrorY) n.setChildMirrrorBit(1, j);
					if (mn.mirrorZ) n.setChildMirrrorBit(2, j);
				}
			}
		}
	}

	_stats.toSDAGTime = _clock.elapsed();
	_stats.nNodesSDAG = _nNodes;
	_state = S_SDAG;

	printf("OK! [%s]\n", sl::human_readable_duration(_stats.toSDAGTime).c_str());
}


bool GeomOctree::checkIntegrity() {
	struct QueueItem {
		QueueItem(id_t id, sl::uint8_t l) : nodeID(id), level(l) {}
		id_t nodeID;
		sl::uint8_t level;
	};
	std::queue<QueueItem> queue;

	queue.push(QueueItem(0, 0));

	unsigned int nVisitedNodes = 0;

	while (!queue.empty()) {
		QueueItem qi = queue.front(); queue.pop();
		nVisitedNodes++;
		if (qi.level >= _levels) return false;
		if (qi.nodeID >= _data[qi.level].size()) return false;
		Node &n = _data[qi.level][qi.nodeID];
		if (!n.hasChildren()) return false;
		for (int i = 0; i < 8; ++i) {
			if (qi.level < _levels - 1) {
				if (n.existsChild(i) && !n.existsChildPointer(i)) return false;
				if (!n.existsChild(i) && n.existsChildPointer(i)) return false;
				if (n.existsChild(i)) queue.push(QueueItem(n.children[i], qi.level+1));
			}
		}
	}

	for (unsigned int i = 0; i < _levels; i++) {
		printf("%u\t%zu\n", i, _data[i].size());
	}

	printf("Visited %i nodes\n", nVisitedNodes);

	return true;
}

/**
 * @brief GeomOctree::invertInvs Unsets mirror bits for axis' that are invariant for child nodes
 * @param n
 * @param lev
 * @param inX
 * @param inY
 * @param inZ
 */
void GeomOctree::invertInvs(Node &n, int lev, bool inX, bool inY, bool inZ) {
	for (int i = 0; i < 8; ++i) {
		if (n.existsChildPointer(i)) {
			Node & nc = _data[lev + 1][n.children[i]];
			if (inX && nc.getInvariantBit(0)) n.unsetChildMirrrorBit(0, i);
			if (inY && nc.getInvariantBit(1)) n.unsetChildMirrrorBit(1, i);
			if (inZ && nc.getInvariantBit(2)) n.unsetChildMirrrorBit(2, i);
		}
	}
}

size_t GeomOctree::getMemFootprint() {
	size_t mem = sizeof(Octree);
	for (int i = 0; i < _data.size(); ++i) {
		mem += _data[i].capacity()*sizeof(Node);
		mem += sizeof(std::vector<Node>);
	}
	return mem;
}

int GeomOctree::traverse(sl::point3f p) const
{
	if (_state == S_EMPTY) return -1;

	if (!_bbox.contains(p)) return -1;

	if (_state == S_SVO || _state == S_DAG) {

		const Node *n = &_data[0][0];
		sl::point3f nodeCenter = _bbox.center();
		unsigned int nodeLevel = 0;

		while (nodeLevel < _levels) {
			int selectedChild = 4 * int(p[0] > nodeCenter[0]) + 2 * int(p[1] > nodeCenter[1]) + int(p[2] > nodeCenter[2]);
			if (!n->existsChild(selectedChild)) break;
			nodeLevel++;
			float hs = getHalfSide(nodeLevel);
			nodeCenter[0] += (p[0] > nodeCenter[0]) ? hs : -1.f * hs;
			nodeCenter[1] += (p[1] > nodeCenter[1]) ? hs : -1.f * hs;
			nodeCenter[2] += (p[2] > nodeCenter[2]) ? hs : -1.f * hs;
			if (nodeLevel < _levels) n = &_data[nodeLevel][ n->children[selectedChild] ];
		}
		return nodeLevel;
	}

	if (_state == S_SDAG) {

		const Node *n = &_data[0][0];
		sl::point3f nodeCenter = _bbox.center();
		unsigned int nodeLevel = 0;
		bool mX = false, mY = false, mZ = false;
		
		while (nodeLevel < _levels) {
			if (mX) p[0] = 2.f * nodeCenter[0] - p[0];
			if (mY) p[1] = 2.f * nodeCenter[1] - p[1];
			if (mZ) p[2] = 2.f * nodeCenter[2] - p[2];
			int selectedChild = 4 * int(p[0] > nodeCenter[0]) + 2 * int(p[1] > nodeCenter[1]) + int(p[2] > nodeCenter[2]);
			if (!n->existsChild(selectedChild)) break;
			nodeLevel++;
			float hs = getHalfSide(nodeLevel);
			nodeCenter[0] += (p[0] > nodeCenter[0]) ? hs : -1.f * hs;
			nodeCenter[1] += (p[1] > nodeCenter[1]) ? hs : -1.f * hs;
			nodeCenter[2] += (p[2] > nodeCenter[2]) ? hs : -1.f * hs;
			mX = n->getChildMirrrorBit(0, selectedChild);
			mY = n->getChildMirrrorBit(1, selectedChild);
			mZ = n->getChildMirrrorBit(2, selectedChild);

			if (nodeLevel < _levels) n = &_data[nodeLevel][n->children[selectedChild]];
		}
		return nodeLevel;
	}

	return -1;
}

