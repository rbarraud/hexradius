#ifndef OR_POWERS_HPP
#define OR_POWERS_HPP

#include <stdint.h>

#include "octradius.hpp"

class Server;
class Client;

const uint32_t PWR_SHIELD = 1<<0;
const uint32_t PWR_CLIMB = 1<<1;
const uint32_t HAS_POWER = 1<<2;
const uint32_t PWR_GOOD = (PWR_SHIELD | PWR_CLIMB);

namespace Powers {
	struct Power {
		const char *name;
		int (*func)(Pawn*, Server*, Client*);
		int spawn_rate;
	};
	
	extern Power powers[];
	extern const int num_powers;
	
	int RandomPower(void);
	
	int destroy_row(Pawn *pawn, Server *server, Client *client);
	int destroy_radial(Pawn *pawn, Server *server, Client *client);
	int destroy_bs(Pawn *pawn, Server *server, Client *client);
	int destroy_fs(Pawn *pawn, Server *server, Client *client);
	
	int raise_tile(Pawn *pawn, Server *server, Client *client);
	int lower_tile(Pawn *pawn, Server *server, Client *client);
	int increase_range(Pawn *pawn, Server *server, Client *client);
	int hover(Pawn *pawn, Server *server, Client *client);
	int shield(Pawn *pawn, Server *server, Client *client);
	
	int elevate_row(Pawn *pawn, Server *server, Client *client);
	int elevate_radial(Pawn *pawn, Server *server, Client *client);
	int elevate_bs(Pawn *pawn, Server *server, Client *client);
	int elevate_fs(Pawn *pawn, Server *server, Client *client);
	
	int dig_row(Pawn *pawn, Server *server, Client *client);
	int dig_radial(Pawn *pawn, Server *server, Client *client);
	int dig_bs(Pawn *pawn, Server *server, Client *client);
	int dig_fs(Pawn *pawn, Server *server, Client *client);
	
	int purify_row(Pawn *pawn, Server *server, Client *client);
	int purify_radial(Pawn *pawn, Server *server, Client *client);
	int purify_bs(Pawn *pawn, Server *server, Client *client);
	int purify_fs(Pawn *pawn, Server *server, Client *client);
}

#endif /* !OR_POWERS_HPP */
