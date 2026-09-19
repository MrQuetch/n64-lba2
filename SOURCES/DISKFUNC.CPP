#include "C_EXTERN.H"
#include "DIRECTORIES.H"

#include <SYSTEM/ADELINE.H>
#include <SYSTEM/LOGPRINT.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
extern "C" void WiiU_PtrFile3DCanary(const char *where);
#endif

#include <errno.h>

U8 *PtrSce;
U8 *PtrScene;
S32 CubeMode = 0;     // 0-->Intérieur - 1-->Extérieur
S32 LastCubeMode = 0; // 0-->Intérieur - 1-->Extérieur

// Scene/cube data on disk is LE (x86 retail). On PPC (Wii U) every multi-byte
// GET_* must byte-swap on read or the parser misaligns and NbObjets/CubeStartX
// etc come out as garbage. Single-byte GET_S8 is endian-agnostic.
#define GET_S8 (*PtrSce++)
#ifdef LBA2_TARGET_N64
// MIPS traps on unaligned loads: the direct *(U16*)/(U32*) casts of the Wii U
// branch would raise an address-error exception whenever PtrSce is odd.
// Read through memcpy instead (GCC lowers it to lwl/lwr pairs on VR4300 —
// unaligned-safe with no function-call overhead), then LE-swap like Wii U.
#include <string.h>
static inline U16 N64ReadLE16(const U8 *p) { U16 v; memcpy(&v, p, 2); return LBA2_LE16(v); }
static inline U32 N64ReadLE32(const U8 *p) { U32 v; memcpy(&v, p, 4); return LBA2_LE32(v); }
#define GET_S16 ((S16)N64ReadLE16(PtrSce)); PtrSce += 2
#define GET_U32 (N64ReadLE32(PtrSce));      PtrSce += 4
#elif defined(LBA2_TARGET_WIIU)
#define GET_S16 ((S16)LBA2_LE16(*(U16 *)PtrSce)); PtrSce += 2
#define GET_U32 (LBA2_LE32(*(U32 *)PtrSce));      PtrSce += 4
#else
#define GET_S16       \
    (*(S16 *)PtrSce); \
    PtrSce += 2
#define GET_U32       \
    (*(U32 *)PtrSce); \
    PtrSce += 4
#endif

/*══════════════════════════════════════════════════════════════════════════*
	     █▀▀▀▄  █    ██▀▀▀ █  ▄▀       █▀▀▀▀ █   █ ██▄ █ █▀▀▀▀
	     ██  █  ██   ▀▀▀▀█ ██▀▄        ██▀▀  ██  █ ██▀██ ██
	     ▀▀▀▀   ▀▀   ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀    ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
/*
void	GetDiskEnv( char *progpath )
{
	_splitpath( (char*)progpath, ProgDrive, ProgDir, Name, Ext ) ;
	getcwd( OrgDir, _MAX_DIR ) ;
	_dos_getdrive( &OrgDrive ) ;
}

void	RestoreDiskEnv()
{
	unsigned total ;

	_dos_setdrive( OrgDrive, &total ) ;
	chdir( OrgDir ) ;
}
*/

/*══════════════════════════════════════════════════════════════════════════*
	  ██▀▀▀ █▀▀▀▀ █▀▀▀▀ ██▄ █ █▀▀▀▀       █▀▀▀▀  █    █     █▀▀▀▀
	  ▀▀▀▀█ ██    ██▀▀  ██▀██ ██▀▀        ██▀▀   ██   ██    ██▀▀
	  ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀     ▀▀   ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
#ifdef LBA2_TARGET_WIIU
// [scenechg] beta=2048 heisenbug: byte-wise (emulator-free) checksum of the
// zone table at load time, to tell at run time a misread by the unaligned-
// access emulator from a real mutation of the zone memory. See GereZoneChangeCube.
U32 g_zoneChkLoad = 0;
S32 g_zoneChkBytes = 0;
#endif

S32 LoadScene(S32 numscene) {
    T_OBJET *ptrobj;
    T_ZONE *ptrz;
    S32 sizetoload;
    S32 indexfile3d;
    U8 n;
    char tmpFilePath[ADELINE_MAX_PATH];
    //        U8	t ;

    // numscene+1 car en 0 se trouve SizeCube.MAX (taille du plus gros .SCC)
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCENE_HQR_NAME);
    if (!Load_HQR(tmpFilePath, PtrScene, numscene + 1)) {
        TheEndCheckFile(tmpFilePath);
    }

    ManageSystem(); // for streaming

    PtrSce = PtrScene;

    // info world: INFO_WORLD

    NewCube = numscene;

    Island = GET_S8;

    Planet = TabArrow[Island].Planet;

    // Sert uniquement pour la 3DExt
    CurrentCubeX = GET_S8;
    CurrentCubeY = GET_S8;

    ShadowLevel = GET_S8;
    ModeLabyrinthe = GET_S8;

    CubeMode = GET_S8;

    if (!FlagLoadGame
            AND(!FlagDrawHorizon OR(CubeMode + LastCubeMode != 2))) {
        FadeToBlackAndSamples(PtrPal);
        HQ_StopSample();

        Cls();
        BoxUpdate();
    }

    n = GET_S8;

    // ambiance: AMBIANCE

    AlphaLight = GET_S16;
    BetaLight = GET_S16;

    SampleAmbiance[0] = GET_S16;
    SampleRepeat[0] = GET_S16;
    SampleRnd[0] = GET_S16;
    SampleFreq[0] = GET_S16;
    SampleVol[0] = GET_S16;
    SampleAmbiance[1] = GET_S16;
    SampleRepeat[1] = GET_S16;
    SampleRnd[1] = GET_S16;
    SampleFreq[1] = GET_S16;
    SampleVol[1] = GET_S16;
    SampleAmbiance[2] = GET_S16;
    SampleRepeat[2] = GET_S16;
    SampleRnd[2] = GET_S16;
    SampleFreq[2] = GET_S16;
    SampleVol[2] = GET_S16;
    SampleAmbiance[3] = GET_S16;
    SampleRepeat[3] = GET_S16;
    SampleRnd[3] = GET_S16;
    SampleFreq[3] = GET_S16;
    SampleVol[3] = GET_S16;
    SecondMin = GET_S16;
    SecondEcart = GET_S16;

    CubeJingle = GET_S8;

    // hero inits: HERO_START

    ptrobj = ListObjet;

    CubeStartX = GET_S16;
    CubeStartY = GET_S16;
    CubeStartZ = GET_S16;

    sizetoload = GET_S16;
    ptrobj->PtrTrack = PtrSce;
    PtrSce += sizetoload;

    sizetoload = GET_S16;
    ptrobj->PtrLife = PtrSce;
    PtrSce += sizetoload;

    ptrobj++;

    // objets: OBJECT

    NbObjets = GET_S16;
#ifdef LBA2_TARGET_WIIU
    LogPrintf("[LoadScene] NbObjets=%d BufferFile3D=%p\n", (int)NbObjets, (void*)BufferFile3D);
#endif
    for (n = 1; n < NbObjets; n++, ptrobj++) {
        InitObject(n);

        ptrobj->Flags = GET_U32;

        indexfile3d = GET_S16;

#ifdef LBA2_TARGET_WIIU
        if (n <= 16) {
            LogPrintf("[LoadScene obj %d] Flags=0x%08X indexfile3d=%d sprite3d=%d\n",
                      (int)n, (unsigned)ptrobj->Flags, (int)indexfile3d,
                      (int)((ptrobj->Flags & SPRITE_3D) ? 1 : 0));
        }
#endif

        if (!(ptrobj->Flags & SPRITE_3D)) {
            if (!FlagLoadGame) {
                ptrobj->PtrFile3D = LoadFile3D(indexfile3d);
            }

            ptrobj->IndexFile3D = indexfile3d;

#ifdef LBA2_TARGET_WIIU
            if (n <= 16 && ptrobj->PtrFile3D != NULL) {
                U8 *p = ptrobj->PtrFile3D;
                LogPrintf("[LoadScene obj %d] PtrFile3D=%p first16=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                          (int)n, (void*)p,
                          p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                          p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            }
#endif
        }

        ManageSystem(); // for streaming

        ptrobj->GenBody = GET_S8;
        ptrobj->GenAnim = GET_S16;
        ptrobj->Sprite = GET_S16;

        ptrobj->OldPosX = ptrobj->Obj.X = GET_S16;
        ptrobj->OldPosY = ptrobj->Obj.Y = GET_S16;
        ptrobj->OldPosZ = ptrobj->Obj.Z = GET_S16;
        ptrobj->HitForce = GET_S8;
        ptrobj->OptionFlags = GET_S16;
        ptrobj->OptionFlags &= ~EXTRA_GIVE_NOTHING;
        ptrobj->Obj.Beta = GET_S16;
        ptrobj->SRot = GET_S16;
        ptrobj->Move = GET_S8;

        if (!(ptrobj->Flags & SPRITE_3D)
                 AND ptrobj->Move != MOVE_WAGON // cf TM_SPEED
                                         AND ptrobj->SRot) {
            // A Priori, ce test ne sert à rien ici
            if (ptrobj->SRot == 1) {
                ptrobj->SRot++;
            }

            ptrobj->SRot = (S16)(51200 / (S32)ptrobj->SRot); // 1024*1000/20
        }

        ptrobj->Info = GET_S16;
        ptrobj->Info1 = GET_S16;
        ptrobj->Info2 = GET_S16;
        ptrobj->Info3 = GET_S16;

        ptrobj->NbBonus = GET_S16;
        ptrobj->CoulObj = GET_S8;

        if (ptrobj->Flags & ANIM_3DS) {
            // num anim 3DS
            ptrobj->Coord.A3DS.Num = GET_U32;
            // NbFps
            ptrobj->Info3 = ptrobj->SizeSHit = GET_S16;
        }

        ptrobj->Armure = GET_S8;
        ptrobj->LifePoint = GET_S8;

        // Message chapter (a virer)
        /*			for( t=0; t<MAX_CHAPTER; t++ )
			{
				ptrobj->MessageChapter[t] = GET_S16 ;
			}
*/
        sizetoload = GET_S16;
        ptrobj->PtrTrack = PtrSce;
        PtrSce = PtrSce + sizetoload;

        sizetoload = GET_S16;
        ptrobj->PtrLife = PtrSce;
        PtrSce = PtrSce + sizetoload;
    }

    Checksum = GET_U32;

    // zone declechement: ZONE

    NbZones = GET_S16;
    ListZone = (T_ZONE *)PtrSce;
    PtrSce += NbZones * sizeof(T_ZONE);

#ifdef LBA2_TARGET_WIIU
    // T_ZONE = 14x S32 (X0..Z1 bbox + Info0..7) + 2x S16 (Type, Num), used as
    // a raw overlay on the LE scene buffer. Left unswapped no zone ever fires:
    // Type garbage skips every case in the switch below and in GereZone, and
    // the bbox test never contains the hero — so ACTION in normal behavior
    // (zone giver/message), change-cube, camera and GRM zones all go dead.
    {
        U32 *zp = (U32 *)ListZone;
        S32 zi;

        for (zi = 0; zi < NbZones; zi++, zp += sizeof(T_ZONE) / 4) {
            U16 *zs = (U16 *)(zp + 14);
            S32 k;

            for (k = 0; k < 14; k++) // X0..Info7
                zp[k] = LBA2_LE32(zp[k]);

            zs[0] = LBA2_LE16(zs[0]); // Type
            zs[1] = LBA2_LE16(zs[1]); // Num
        }
    }
#endif

    ptrz = ListZone;

    for (n = 0; n < NbZones; n++, ptrz++) {
        switch (ptrz->Type) {
        case 0: // change cube
            if (ptrz->Info5 & 1)
                ptrz->Info5 |= ZONE_TEST_BRICK;
            else
                ptrz->Info5 &= ~(ZONE_TEST_BRICK);

            if (ptrz->Info6 & 1)
                ptrz->Info6 |= ZONE_DONT_REAJUST_POS_TWINSEN;
            else
                ptrz->Info6 &= ~(ZONE_DONT_REAJUST_POS_TWINSEN);
        case 1: // camera
            if (ptrz->Info7 & 1)
                ptrz->Info7 |= ZONE_ON;
            else
                ptrz->Info7 &= ~(ZONE_ON);

            ptrz->Info7 &= ~(ZONE_ACTIVE);
            break;

        case 3: // zone grm
            ptrz->Info1 = 0;
            break;

        case 4:              /* zone giver */
            ptrz->Info2 = 0; /* pas encore pris */
            break;

        case 6: // echelle
        case 9: // Rail
            ptrz->Info1 = ptrz->Info0;
            break;

        case 8:              // zone Hit
            ptrz->Info3 = 0; // init TimerRef
            break;
        }
    }

#ifdef LBA2_TARGET_WIIU
    // [scenechg] Layout of the cube-change (Type 0) / camera (Type 1) zones of
    // the loaded scene: compares the cellar exits between the 1st and 2nd visit.
    {
        T_ZONE *zd = ListZone;
        S32 zn;
        for (zn = 0; zn < NbZones; zn++, zd++) {
            if (zd->Type == 0 OR zd->Type == 1)
                LogPrintf("[scenechg] LoadScene(cube=%d) zone idx=%d Type=%d Num=%d bbox={%d,%d,%d..%d,%d,%d} Info0=%d Info1=%d Info2=%d Info3=%d\n",
                          (int)NewCube, (int)zn, (int)zd->Type, (int)zd->Num,
                          (int)zd->X0, (int)zd->Y0, (int)zd->Z0,
                          (int)zd->X1, (int)zd->Y1, (int)zd->Z1,
                          (int)zd->Info0, (int)zd->Info1, (int)zd->Info2, (int)zd->Info3);
        }

        // FNV-1a byte-wise (U8 reads NEVER go through the unaligned-access
        // emulator => reflects the real memory contents).
        {
            U8 *zb = (U8 *)ListZone;
            S32 nby = NbZones * (S32)sizeof(T_ZONE);
            U32 ck = 0x811c9dc5u;
            for (S32 bi = 0; bi < nby; bi++)
                ck = (ck ^ zb[bi]) * 16777619u;
            g_zoneChkLoad = ck;
            g_zoneChkBytes = nby;
            LogPrintf("[scenechg] LoadScene(cube=%d) zoneCRC=%08X bytes=%d base=%p\n",
                      (int)NewCube, (unsigned)ck, (int)nby, (void *)ListZone);
        }
    }
#endif

    // point track: TRACK

    NbBrickTrack = GET_S16;
    ListBrickTrack = (T_TRACK *)PtrSce;
    PtrSce += NbBrickTrack * sizeof(T_TRACK);
#ifdef LBA2_TARGET_WIIU
    // T_TRACK = {S32 X, S32 Y, S32 Z} per entry, all LE on disk. Without this
    // swap LM_POS_POINT (opcode 0x3A) in DoLife writes garbage S32 values into
    // Twinsen.Obj.X/Y/Z on the first iter — confirmed via per-opcode trace.
    {
        S32 *p = (S32 *)ListBrickTrack;
        for (S32 i = 0; i < NbBrickTrack; ++i) {
            p[i*3 + 0] = LBA2_LE32(p[i*3 + 0]);
            p[i*3 + 1] = LBA2_LE32(p[i*3 + 1]);
            p[i*3 + 2] = LBA2_LE32(p[i*3 + 2]);
        }
    }
#endif

    //--------- PATCHES ----------
    NbPatches = GET_U32;
    ListPatches = (T_PATCH *)PtrSce;
#ifdef LBA2_TARGET_WIIU
    // T_PATCH = {S16 Size, S16 Offset}, raw LE overlay. The save-game writer
    // walks these and reads PtrScene + Offset for Size bytes — byte-swapped
    // garbage there means a wild read/copy (the "save crashes" symptom).
    {
        U16 *pp = (U16 *)ListPatches;
        S32 pi;

        for (pi = 0; pi < NbPatches * 2; pi++)
            pp[pi] = LBA2_LE16(pp[pi]);
    }
#endif
    //----------------------------

    if (NumCube == NUM_CUBE_PHANTOM) {
        Island = PhantomIsland;
        CurrentCubeX = PhantomX;
        CurrentCubeY = PhantomY;
    }

    ManageSystem(); // for streaming

    ChoicePalette();

    //		FlagFade = TRUE ;

#ifdef LBA2_TARGET_WIIU
    WiiU_PtrFile3DCanary("LoadScene before PtrInit3DGame");
#endif

    // Init suivant type de cube
    PtrInit3DGame();

#ifdef LBA2_TARGET_WIIU
    WiiU_PtrFile3DCanary("LoadScene after PtrInit3DGame, before return");
#endif

    return TRUE;
}

/*──────────────────────────────────────────────────────────────────────────*/
// Pour le buggy pour recupérer les coordonnées 2D d'un cube extérieur dans
// la map
S32 LoadSceneCubeXY(S32 numcube, S32 *cubex, S32 *cubey) {
    S32 x, y;
    U8 island, shadowlvl, modelaby, cubemode;
    char tmpFilePath[ADELINE_MAX_PATH];

    // ATTENTION: chargée dans ScreenAux car normalement appelée dans le
    //	      comportement d'init du buggy

    // numscene+1 car en 0 se trouve SizeCube.MAX (taille du plus gros .SCC)
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCENE_HQR_NAME);
    if (!Load_HQR(tmpFilePath, ScreenAux, numcube + 1)) {
        TheEndCheckFile(tmpFilePath);
    }

    *cubex = *cubey = 0;

    PtrSce = ScreenAux;

    // info world: INFO_WORLD

    island = GET_S8;

    // Sert uniquement pour la 3DExt
    x = GET_S8;
    y = GET_S8;

    shadowlvl = GET_S8;
    modelaby = GET_S8;
    cubemode = GET_S8;

    if (cubemode == CUBE_EXTERIEUR AND island == Island AND abs(x - CurrentCubeX) <= 1 AND abs(y - CurrentCubeY) <= 1) {
        *cubex = x;
        *cubey = y;

        return TRUE;
    } else
        return FALSE;
}

/*──────────────────────────────────────────────────────────────────────────*/

U8 AddDirIfNot(const char *path) {
    if (ExistsFileOrDir(path)) {
        return IsDirectory(path);
    }

    char *copypath = strdup(path);

    { // If path ends in separator, remove it
        size_t copyLen = strlen(copypath);
        if ((copyLen > 0) && (copypath[copyLen - 1] == ADELINE_PATH_SEP_CHAR)) {
            copypath[copyLen - 1] = '\0';
        }
    }

    // Create each missing component in turn. An intermediate mkdir failure
    // is NOT conclusive: on Wii U the leading components are virtual mounts
    // (mkdir("/vol") fails with an errno that is not EEXIST — the old
    // status/errno chain broke there and the final directory was never
    // created, so game saves silently failed to persist). What matters is
    // whether each component exists, and whether the full path is a
    // directory at the end.
    char *pp = copypath;
    char *sp;
    while ((sp = strchr(pp, ADELINE_PATH_SEP_CHAR)) != NULL) {
        if (sp != pp) {
            *sp = '\0';
            if (!ExistsFileOrDir(copypath)) {
                ADELINE_MKDIR(copypath);
            }
            *sp = ADELINE_PATH_SEP_CHAR;
        }
        pp = sp + 1;
    }

    if (!ExistsFileOrDir(copypath)) {
        ADELINE_MKDIR(copypath);
    }

    U8 ok = (U8)(ExistsFileOrDir(copypath) AND IsDirectory(copypath));

    free(copypath);

    return ok;
}

/*──────────────────────────────────────────────────────────────────────────*/

void CreateLbaDirectories(void) {
    char tmpDirPath[ADELINE_MAX_PATH];

#ifndef DEMO
    GetVoicePath(tmpDirPath, ADELINE_MAX_PATH, NULL);
    AddDirIfNot(tmpDirPath);
#endif

    GetSavePath(tmpDirPath, ADELINE_MAX_PATH, NULL);
    if (!AddDirIfNot(tmpDirPath)) {
        LogPrintf("Warning: could not create save dir '%s'\n", tmpDirPath);
    }

    GetShootPath(tmpDirPath, ADELINE_MAX_PATH, NULL);
    AddDirIfNot(tmpDirPath);

#ifdef DEBUG_TOOLS
    GetBugPath(tmpDirPath, ADELINE_MAX_PATH, NULL);
    if (!ExistsFileOrDir(tmpDirPath)) {
        if (AddDirIfNot(tmpDirPath)) {
            LogPrintf("Created bugs directory: %s\n", tmpDirPath);
        }
    }
#endif

    GetLogPath(tmpDirPath, ADELINE_MAX_PATH, NULL);
    AddDirIfNot(tmpDirPath);
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
