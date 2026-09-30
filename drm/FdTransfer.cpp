#include "FdTransfer.h"


// Upstream RadeonGfx used _kern_dup_foreign(), a syscall from a patched Haiku
// that is not in upstream Haiku. Buffer and syncobj export/import (prime and
// sync_file fds) need it; until the display driver provides an equivalent,
// report it as unsupported.
// TODO: implement with an ioctl on the radeon_hd device (kernel side:
// dup_foreign_fd()).
int
dup_foreign_fd(team_id fromTeam, team_id toTeam, int fd, int flags)
{
	(void)fromTeam; (void)toTeam; (void)fd; (void)flags;
	return B_NOT_SUPPORTED;
}
