#pragma pass outline_mask vertex fragment

// The editor's selection mask: the silhouette and depth of whatever the
// editor has selected, drawn into the outline's seed buffer for
// render/outline.slang to flood and composite. The body is the engine's
// (engine/outline.slang); this file exists only because the editor outlines
// objects that keep their own materials, so the mask cannot come from them
// the way a game's does -- one shader the editor draws every selected mesh
// through, rather than a pass added to every material's shader.
//
// Editor-only: the standalone never loads it.

#include "engine/outline.slang"
