#ifndef OR_TILE_ANIMS_HPP
#define OR_TILE_ANIMS_HPP

#include "octradius.hpp"

#undef ABSOLUTE
#undef RELATIVE

class Client;

namespace TileAnimators {
	struct Animator {
		Tile::List tiles;
		unsigned int start_time;
		unsigned int last_time;

		Animator(Tile::List _tiles);
		// Returns true if the animation still has stuff to do.
		// If false, the client will stop running & delete the animation.
		virtual bool do_stuff() = 0;
		virtual ~Animator();
	};

	enum ElevationMode { ABSOLUTE, RELATIVE };

	struct ElevationAnimator: public Animator {
		ElevationAnimator(Tile::List _tiles, Tile* center, float delay_factor, ElevationMode mode, int target_elevation);
		virtual bool do_stuff();
	};
}

#endif
