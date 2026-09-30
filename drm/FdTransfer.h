#pragma once

#include <OS.h>


// Duplicates file descriptor fd of team fromTeam into team toTeam and returns
// the new descriptor (in toTeam), or an error code.
int dup_foreign_fd(team_id fromTeam, team_id toTeam, int fd, int flags);
