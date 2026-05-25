//===- ConsGEdge.h -- Constraint graph edge-----------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2017>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * ConsGEdge.h
 *
 *  Created on: Mar 19, 2014
 *      Author: Yulei Sui
 */

#ifndef CONSGEDGE_H_
#define CONSGEDGE_H_

#include "SVFIR/SVFIR.h"
#include "Util/WorkList.h"

#include <map>
#include <set>

namespace SVF
{

class PathCond;
class ConstraintNode;
/*!
 * Self-defined edge for constraint resolution
 * including add/remove/re-target, but all the operations do not affect original SVFIR Edges
 */
typedef GenericEdge<ConstraintNode> GenericConsEdgeTy;
class ConstraintEdge : public GenericConsEdgeTy
{

public:
    /// five kinds of constraint graph edges
    /// Gep edge is used for field sensitivity
    enum ConstraintEdgeK
    {
        Addr, Copy, Store, Load, NormalGep, VariantGep
    };
private:
    EdgeID edgeId;
    const PathCond* guard;  ///< Conditional guard for path-aware analysis
public:
    /// Constructor
    ConstraintEdge(ConstraintNode* s, ConstraintNode* d, ConstraintEdgeK k, EdgeID id = 0, const PathCond* g = nullptr)
        : GenericConsEdgeTy(s,d,k), edgeId(id), guard(g)
    {
    }
    /// Destructor
    ~ConstraintEdge()
    {
    }
    /// Return edge ID
    inline EdgeID getEdgeID() const
    {
        return edgeId;
    }
    /// Return conditional guard (may be nullptr)
    inline const PathCond* getGuard() const
    {
        return guard;
    }
    /// Set conditional guard
    inline void setGuard(const PathCond* g)
    {
        guard = g;
    }
    /// ClassOf
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == Addr ||
               edge->getEdgeKind() == Copy ||
               edge->getEdgeKind() == Store ||
               edge->getEdgeKind() == Load ||
               edge->getEdgeKind() == NormalGep ||
               edge->getEdgeKind() == VariantGep;
    }
    /// Constraint edge type
    typedef GenericNode<ConstraintNode,ConstraintEdge>::GEdgeSetTy ConstraintEdgeSetTy;

};


/*!
 * Copy edge
 */
class AddrCGEdge: public ConstraintEdge
{
private:
    AddrCGEdge();                      ///< place holder
    AddrCGEdge(const AddrCGEdge &);  ///< place holder
    void operator=(const AddrCGEdge &); ///< place holder
public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const AddrCGEdge *)
    {
        return true;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == Addr;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == Addr;
    }
    //@}

    /// constructor
    AddrCGEdge(ConstraintNode* s, ConstraintNode* d, EdgeID id, const PathCond* g = nullptr);
};


/*!
 * Copy edge
 */
class CopyCGEdge: public ConstraintEdge
{
private:
    CopyCGEdge();                      ///< place holder
    CopyCGEdge(const CopyCGEdge &);  ///< place holder
    void operator=(const CopyCGEdge &); ///< place holder
public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const CopyCGEdge *)
    {
        return true;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == Copy;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == Copy;
    }
    //@}

    /// constructor
    CopyCGEdge(ConstraintNode* s, ConstraintNode* d, EdgeID id, const PathCond* g = nullptr)
        : ConstraintEdge(s,d,Copy,id,g)
    {
    }
};


/*!
 * Store edge
 */
class StoreCGEdge: public ConstraintEdge
{
private:
    StoreCGEdge();                      ///< place holder
    StoreCGEdge(const StoreCGEdge &);  ///< place holder
    void operator=(const StoreCGEdge &); ///< place holder

public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const StoreCGEdge *)
    {
        return true;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == Store;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == Store;
    }
    //@}

    /// constructor
    StoreCGEdge(ConstraintNode* s, ConstraintNode* d, EdgeID id, const PathCond* g = nullptr)
        : ConstraintEdge(s,d,Store,id,g)
    {
    }
};


/*!
 * Load edge
 */
class LoadCGEdge: public ConstraintEdge
{
private:
    LoadCGEdge();                      ///< place holder
    LoadCGEdge(const LoadCGEdge &);  ///< place holder
    void operator=(const LoadCGEdge &); ///< place holder

public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const LoadCGEdge *)
    {
        return true;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == Load;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == Load;
    }
    //@}

    /// Constructor
    LoadCGEdge(ConstraintNode* s, ConstraintNode* d, EdgeID id, const PathCond* g = nullptr)
        : ConstraintEdge(s,d,Load,id,g)
    {
    }
};


/*!
 * Gep edge
 */
class GepCGEdge: public ConstraintEdge
{
private:
    GepCGEdge();                      ///< place holder
    GepCGEdge(const GepCGEdge &);  ///< place holder
    void operator=(const GepCGEdge &); ///< place holder

protected:

    /// Constructor
    GepCGEdge(ConstraintNode* s, ConstraintNode* d, ConstraintEdgeK k, EdgeID id, const PathCond* g = nullptr)
        : ConstraintEdge(s,d,k,id,g)
    {

    }

public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const GepCGEdge *)
    {
        return true;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == NormalGep ||
               edge->getEdgeKind() == VariantGep;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == NormalGep ||
               edge->getEdgeKind() == VariantGep;
    }
    //@}

};

/*!
 * Gep edge with fixed offset size
 */
class NormalGepCGEdge : public GepCGEdge
{
private:
    NormalGepCGEdge();                      ///< place holder
    NormalGepCGEdge(const NormalGepCGEdge &);  ///< place holder
    void operator=(const NormalGepCGEdge &); ///< place holder

    AccessPath ap;	///< Access path of the gep edge

public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const NormalGepCGEdge *)
    {
        return true;
    }
    static inline bool classof(const GepCGEdge *edge)
    {
        return edge->getEdgeKind() == NormalGep;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == NormalGep;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == NormalGep;
    }
    //@}

    /// Constructor
    NormalGepCGEdge(ConstraintNode* s, ConstraintNode* d, const AccessPath& ap,
                    EdgeID id, const PathCond* g = nullptr)
        : GepCGEdge(s, d, NormalGep, id, g), ap(ap)
    {
    }

    /// Get location set of the gep edge
    inline const AccessPath& getAccessPath() const
    {
        return ap;
    }

    /// Get location set of the gep edge
    inline APOffset getConstantFieldIdx() const
    {
        return ap.getConstantStructFldIdx();
    }

};

/*!
 * Gep edge with variant offset size
 */
class VariantGepCGEdge : public GepCGEdge
{
private:
    VariantGepCGEdge();                      ///< place holder
    VariantGepCGEdge(const VariantGepCGEdge &);  ///< place holder
    void operator=(const VariantGepCGEdge &); ///< place holder

public:
    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const VariantGepCGEdge *)
    {
        return true;
    }
    static inline bool classof(const GepCGEdge *edge)
    {
        return edge->getEdgeKind() == VariantGep;
    }
    static inline bool classof(const ConstraintEdge *edge)
    {
        return edge->getEdgeKind() == VariantGep;
    }
    static inline bool classof(const GenericConsEdgeTy *edge)
    {
        return edge->getEdgeKind() == VariantGep;
    }
    //@}

    /// Constructor
    VariantGepCGEdge(ConstraintNode* s, ConstraintNode* d, EdgeID id, const PathCond* g = nullptr)
        : GepCGEdge(s,d,VariantGep,id,g)
    {}
};

} // End namespace SVF

#endif /* CONSGEDGE_H_ */
