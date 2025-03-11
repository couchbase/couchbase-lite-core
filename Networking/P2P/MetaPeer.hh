//
// Created by Jens Alfke on 3/10/25.
//

#pragma once
#include "Base.hh"
#include "c4DatabaseTypes.h" // for C4UUID and its hash method
#include "HTTPListener.hh"
#include "SmallVector.hh"
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

class C4Peer;

namespace litecore::p2p {

    /// Returns true if the direction from A to B is clockwise, when interpreting UUIDs as 128-bit
    /// big-endian integers and mapping them around a clock face with 0000.. and FFFF... touching.
    /// That is, the distance from A clockwise to B is less than from B clockwise to A.
    ///
    /// Why is this useful? It's a fair and deterministic way to choose between two UUIDs, such that
    /// any specific UUID will be chosen against 50% of other UUIDs. (If we used ">" to compare,
    /// low-numbered UUIDs would rarely be chosen.)
    bool clockwise(C4UUID const& a, C4UUID const& b) noexcept;


    /** Represents a peer device, independent of protocol/provider.
        A union of one or more C4Peers that have the same UUID. */
    class MetaPeer : public RefCounted {
    public:
        using Task = REST::HTTPListener::Task;

        MetaPeer(C4UUID const& id, C4Peer* c4peer);

        C4UUID const uuid;

        size_t count() const noexcept {return _c4peers.size();}
        bool empty() const noexcept {return _c4peers.empty();}

        /// The best peer to connect to, if any.
        C4Peer* bestC4Peer() const;

        size_t taskCount() const noexcept {return _tasks.size();}

        void addTask(Task*);
        void removeTask(Task*);

    private:
        friend class MetaPeers;
        bool addC4Peer(Retained<C4Peer> const& c4peer);
        bool removeC4Peer(Retained<C4Peer> const& c4peer);

        fleece::smallVector<Retained<C4Peer>, 2> _c4peers;
        fleece::smallVector<Retained<Task>, 2> _tasks;
    };


    /** A set of MetaPeers. */
    class MetaPeers {
    public:
        /// Returns the MetaPeer with the given UUID, or nullptr.
        Retained<MetaPeer> operator[](C4UUID const& id) const;

        /// Returns the MetaPeer the given C4Peer belongs to, or nullptr.
        Retained<MetaPeer> metaPeerWithC4Peer(C4Peer*) const;

        /// The number of MetaPeers.
        size_t size() const {return _metaPeers.size();}

        auto begin() const {return _metaPeers.begin();}
        auto end() const {return _metaPeers.end();}

        /// Assigns the C4Peer to a MetaPeer with this C4UUID, creating one if necessary.
        /// Returns the MetaPeer, or nullptr if nothing changed.
        /// (You can tell if the MetaPeer is new: its `count` will be 1.)
        Retained<MetaPeer> addC4Peer(C4Peer*, C4UUID const&);

        /// Removes this C4Peer from its MetaPeer.
        /// Returns the MetaPeer, or nullptr if nothing changed.
        /// (You can tell if the MetaPeer itself was removed: its `empty` property will be true.)
        Retained<MetaPeer> removeC4Peer(C4Peer*);

    private:
        std::unordered_map<C4UUID,Retained<MetaPeer>> _metaPeers;   // UUID -> MetaPeer
        std::unordered_map<std::string,C4UUID> _c4uuids;            // C4Peer::id -> UUID
    };

}

C4_ASSUME_NONNULL_END
