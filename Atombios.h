#pragma once

#include "atombios/atom.h"
#include <AutoDeleterOS.h>

struct radeon_shared_info;


class Atombios {
private:
	AreaDeleter fBiosArea;
	uint8 *fBios;
	card_info fCardInfo;
	CObjectDeleter<atom_context, void, atom_destroy> fContext;

public:
	Atombios();
	status_t Init();
	status_t Init(area_id romArea);
	bool IsPosted();

	inline atom_context *Context() {return fContext.Get();}
};
