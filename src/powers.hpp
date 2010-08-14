#ifndef OR_POWERS_HPP
#define OR_POWERS_HPP

#include "octradius.hpp"

const int PWR_ARMOUR = 1<<0;
const int PWR_CLIMB = 1<<1;
const int PWR_GOOD = (PWR_ARMOUR | PWR_CLIMB);

namespace Powers {
	struct Power {
		const char *name;
		int (*func)(Pawn*);
		int spawn_rate;
	};
	
	extern const Power powers[];
	extern const int num_powers;
	
	int destroy_column(Pawn *pawn);
	int destroy_row(Pawn *pawn);
	int destroy_radial(Pawn *pawn);
	int raise_tile(Pawn *pawn);
	int lower_tile(Pawn *pawn);
	int moar_range(Pawn *pawn);
	int climb_tile(Pawn *pawn);
	int wall_column(Pawn *pawn);
	int wall_row(Pawn *pawn);
	int armour(Pawn *pawn);
	int purify_row(Pawn *pawn);
	int purify_column(Pawn *pawn);
	int purify_radial(Pawn *pawn);
}

#endif /* !OR_POWERS_HPP */
