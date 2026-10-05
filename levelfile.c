/*  :ts=8 bk=0
 *
 * levelfile.c:	Routines to load a level.
 *
 * Leo L. Schwab					9305.17
 */
#include <types.h>
#include <mem.h>
#include <operamath.h>
#include <graphics.h>
#include <ctype.h>

#include "castle.h"
#include "objects.h"

#include "app_proto.h"


 /***************************************************************************
  * Globals.
  */
extern MapEntry	chardef[];

extern Object** obtab;
extern int32	obtabsiz;

extern Vertex	playerpos;
extern frac16	playerdir;

extern char	wallimagefile[];
extern char	spoolmusicfile[];

extern int32	floorcolor, ceilingcolor;


MapEntry	levelmap[WORLDSIZ][WORLDSIZ];


static char	commanewline[] = ",\n\r";
static char	commaspace[] = ", ";


/***************************************************************************
 * Code.
 */
int
loadlevelmap(filename)
char* filename;
{
	int32		numobs;
	{
		register int	x, z, len;
		register uint8	c, * cp;
		ObDef* od;
		intptr_t	err_len;
		int		eol;
		int		r, g, b;
		char		rs[8], gs[8], bs[8];
		void* lvlbuf;

		if (!(lvlbuf = allocloadfile(filename, 0, &err_len))) {
			filerr(filename, err_len);
			return (FALSE);
		}

		/*
		 * Gather filename for background music, wall images, and colors for
		 * floor and ceiling.
		 */
		len = (int)err_len;
		cp = lvlbuf;
		numobs = 0;

		cp = snarfstr(cp, spoolmusicfile, commanewline);
		cp = snarfstr(cp, wallimagefile, commanewline);

		cp = snarfstr(cp, rs, commaspace);
		cp = snarfstr(cp, gs, commaspace);
		cp = snarfstr(cp, bs, commanewline);
		r = strtol(rs, NULL, 0);
		g = strtol(gs, NULL, 0);
		b = strtol(bs, NULL, 0);
		ceilingcolor = MakeRGB15Pair(r, g, b);

		cp = snarfstr(cp, rs, commaspace);
		cp = snarfstr(cp, gs, commaspace);
		cp = snarfstr(cp, bs, commanewline);
		r = strtol(rs, NULL, 0);
		g = strtol(gs, NULL, 0);
		b = strtol(bs, NULL, 0);
		floorcolor = MakeRGB15Pair(r, g, b);

		ceilingcolor = MakeRGB15Pair(3, 3, 3);
		floorcolor = MakeRGB15Pair(4, 4, 4);

		len -= (int32)((char*)cp - (char*)lvlbuf);

		/*
		 * Perform initial scan and count number of objects present.
		 */
		for (z = WORLDSIZ; --z >= 0; ) {
			eol = FALSE;
			for (x = 0; x < WORLDSIZ; x++) {
				if (len <= 0)
					eol = TRUE;

				if (eol)
					levelmap[z][x] = chardef['#' - ' '];
				else {
					c = *cp++ & 0x7F;
					len--;

					if (c == '\n' || c == '\r') {
						eol = TRUE;
						continue;
					}

					if (c < ' ') {
						kprintf("Invalid character (0x%02x) at %d,%d", c, x, z);
						die(" in levelmap file.\n");
					}

					levelmap[z][x] = chardef[c - ' '];
					if (od = (ObDef*)chardef[c - ' '].me_Obs) {
						od->od_ObCount--;
						numobs++;
					}
				}

				/*
				 * Set initial player location.
				 */
				if (c == '<' || c == '>' ||
					c == '^' || c == 'v')
				{
					playerpos.X = Convert32_F16(x) + HALF_F16;
					playerpos.Z = Convert32_F16(z) + HALF_F16;

					switch (c) {
					case '^':
						playerdir = 0;
						break;
					case '<':
						playerdir = Convert32_F16(64);
						break;
					case 'v':
						playerdir = Convert32_F16(128);
						break;
					case '>':
						playerdir = Convert32_F16(192);
						break;
					}
				}
			}
			/*
			 * Flush remainder of line or file.
			 */
			if (!eol)
				while (len && (len--, c = *cp++) &&
					c != '\n' && c != '\r')
					;

		}
		FreeMem(lvlbuf, (int32)err_len);

	}
	{
		register MapEntry* me;
		register ObDef* od;
		register Object* ob, ** obt;
		register int		x, z;
		InitData		id;

		/*
		 * Allocate object table.
		 */

		numobs++; // ### HACK: head test (leave room for one extra object)

		if (!(obtab = malloctype(sizeof(Object*) * numobs, MEMTYPE_FILL)))
			die("Can't allocate object table.\n");
		obtabsiz = numobs;

		obtab[numobs - 1] = 0; // ### HACK: head (set last object to 0)

		initobdefs();

		/*
		 * Initialize MapEntries and create objects.
		 */
		obt = obtab;
		for (z = WORLDSIZ; --z >= 0; ) {
			for (x = WORLDSIZ; --x >= 0; ) {
				me = &levelmap[z][x];

				//			cd = &chardef[me->me_Flags];
				//			*me = *cd;

							/*
							 * MapEntries initially contain a pointer to the
							 * ObDef.  This is used to generate an instance.
							 */
				if (!(od = (ObDef*)me->me_Obs))
					continue;

				id.id_MapEntry = me;
				//			id.id_DefEntry	= cd;	May return someday...
				id.id_XIdx = x;
				id.id_ZIdx = z;

				if (!(ob = (Object*)(intptr_t)((od->od_Func) (od,
					OP_CREATEOB,
					NULL))))
					die("Can't create object.\n");

				if ((od->od_Func) (ob, OP_INITOB, &id) < 0)
					die("Error initializing object.\n");

				me->me_Obs = ob;		// Stomp over ObDef
				me->me_Flags |= MEF_ARTWORK;	// Do I need this?

				*obt++ = ob;
			}
		}


		dropfaces();

		return (TRUE);
	}
}


void
dropfaces()
{
	register MapEntry* me;
	register int		i, n, flags;

	for (i = WORLDSIZ; --i >= 0; ) {
		for (n = WORLDSIZ; --n >= 0; ) {
			me = &levelmap[i][n];
			if (!((flags = me->me_VisFlags) & VISF_ALLDIRS))
				continue;

			if (i < WORLDSIZ - 1 &&
				((levelmap[i + 1][n].me_Flags & MEF_OPAQUE) ||
					(levelmap[i + 1][n].me_Obs &&
						levelmap[i + 1][n].me_Obs->ob_Type == OTYP_EWDOOR)))
				flags &= ~VISF_NORTH;

			if (i &&
				((levelmap[i - 1][n].me_Flags & MEF_OPAQUE) ||
					(levelmap[i - 1][n].me_Obs &&
						levelmap[i - 1][n].me_Obs->ob_Type == OTYP_EWDOOR)))
				flags &= ~VISF_SOUTH;

			if (n < WORLDSIZ - 1 &&
				((levelmap[i][n + 1].me_Flags & MEF_OPAQUE) ||
					(levelmap[i][n + 1].me_Obs &&
						levelmap[i][n + 1].me_Obs->ob_Type == OTYP_NSDOOR)))
				flags &= ~VISF_EAST;

			if (n &&
				((levelmap[i][n - 1].me_Flags & MEF_OPAQUE) ||
					(levelmap[i][n - 1].me_Obs &&
						levelmap[i][n - 1].me_Obs->ob_Type == OTYP_NSDOOR)))
				flags &= ~VISF_WEST;

			me->me_VisFlags = flags;
		}
	}
}


void
freelevelmap()
{
	freeobjects();
	destructobdefs();
}



char*
snarfstr(s, dest, terminators)
register char* s, * dest;
char* terminators;
{
	register char	c;

	while (isspace(*s))
		s++;

	while (c = *dest++ = *s++)
		if (strchr(terminators, c))
			break;

	*--dest = '\0';

	return (s);
}
