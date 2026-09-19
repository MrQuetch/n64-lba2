/*──────────────────────────────────────────────────────────────────────────*/
#include <SYSTEM/ADELINE.H>
#include <SYSTEM/TIMER.H>
#include <SYSTEM/N_MALLOC.H>
#include <SYSTEM/LZ.H>
#include <SYSTEM/FILES.H>
#include <SYSTEM/HQFILE.H>
#include <SYSTEM/HQRRESS.H>

#include <string.h>
#include <stdio.h>

//──────────────────────────────────────────────────────────────────────────
typedef struct {
    S32 Index;
    void *Ptr;
    S32 Size;
    U32 Time;
} T_HQR_BLOC;

//──────────────────────────────────────────────────────────────────────────
HQR_GET_CALLBACK *HQRGetErrorFunc = NULL;
HQR_GET_CALLBACK *HQRGetDelFunc = NULL;

//──────────────────────────────────────────────────────────────────────────
S32 HQR_Flag; // flag "load done" par HQR_Get

//──────────────────────────────────────────────────────────────────────────
// initialise la gestion bufferisée d'une ressource
// hqrname: nom du .HQR contenant les fiches compactées
// maxsize: taille buffer memoire
// MaxIndex: nombre de fiche max stocké
// (devrait être > maxsize/average file size )
T_HQR_HEADER *HQR_Init_Ressource(const char *hqrname,
                                 S32 maxsize,
                                 S32 maxrsrc) {
    T_HQR_HEADER *header;
    void *buffer;

    header = (T_HQR_HEADER *)Malloc(sizeof(T_HQR_HEADER) +
                                    sizeof(T_HQR_BLOC) * maxrsrc);

    if (!header) {
        return NULL;
    }

    buffer = Malloc(maxsize + RECOVER_AREA);
    if (!buffer) {
    error:
        Free(header);
        return NULL;
    }

    header->MaxSize = maxsize;
    header->MaxIndex = maxrsrc;
    header->Buffer = buffer;

    if (!HQR_Change_Ressource(header, hqrname)) {
        Free(buffer);
        goto error;
    }

    return header; // header
}

//──────────────────────────────────────────────────────────────────────────
// vide le buffer d'une ressource
void HQR_Reset_Ressource(T_HQR_HEADER *header) {
    header->FreeSize = header->MaxSize;
    header->NbIndex = 0;
}

//──────────────────────────────────────────────────────────────────────────
// change le nom du fichiers HQR qui contient les fiches (vide le buffer)
S32 HQR_Change_Ressource(T_HQR_HEADER *header, const char *newhqrname) {
    if (newhqrname && newhqrname[0]) {
        strcpy(header->Name, newhqrname);

        if (!FileSize(newhqrname)) {
            return FALSE;
        }

    } else {
        header->Name[0] = 0;
    }

    HQR_Reset_Ressource(header);

    return TRUE;
}

//──────────────────────────────────────────────────────────────────────────
// libère la mémoire allouée à la gestion bufferisée d'une ressource HQR
void HQR_Free_Ressource(T_HQR_HEADER *header) {
    if (!header) {
        return;
    }

    /*AIL_vmm_unlock((void*)header, sizeof(T_HQR_HEADER) +
				  sizeof(T_HQR_BLOC) * header->MaxIndex ) ;

	AIL_vmm_unlock(header->Buffer, header->MaxSize+RECOVER_AREA);
	*/
    Free(header->Buffer);
    Free(header);
}

//──────────────────────────────────────────────────────────────────────────
// retourne le pointeur mémoire de la fiche (index) demandée
T_HQR_BLOC *HQR_GiveBloc(S32 index, S32 nbindex, T_HQR_BLOC *bloc) {
    if (nbindex > 0) {
        T_HQR_BLOC *endOfBlocs = &bloc[nbindex];
        for (S32 n = 0; &bloc[n] != endOfBlocs; ++n) {
            T_HQR_BLOC *currBloc = &bloc[n];
            if (currBloc->Index == index) {
                return currBloc;
            }
        }
    }

    return NULL;
}

//──────────────────────────────────────────────────────────────────────────
// supprime une fiche dans le buffer d'une ressource
static inline void HQR_Del_Bloc(T_HQR_HEADER *header, S32 index) {
    S32 n;
    T_HQR_BLOC *ptrbloc;
    S32 delsize;
    void *ptrs, *ptrd;

    ptrbloc = (T_HQR_BLOC *)(header + 1);
    delsize = ptrbloc[index].Size;

    // if this is last index then skip this...
    if (index < header->NbIndex - 1) {
        // shift buffer
        ptrd = ptrbloc[index].Ptr;
        ptrs = (void *)((U8 *)ptrd + delsize);
        memmove(ptrd, ptrs, (header->MaxSize - header->FreeSize) - ((U64)ptrs - (U64)(header->Buffer)));

        // shift index table
        ptrd = (void *)&ptrbloc[index];
        ptrs = (void *)&ptrbloc[index + 1];
        memmove(ptrd, ptrs, (header->NbIndex - index - 1) * sizeof(T_HQR_BLOC));

        // shift index value
        for (n = index; n < (header->NbIndex - 1); n++) {
            *(S32 *)&(ptrbloc[n].Ptr) -= delsize;
        }
    }

    // update buffer status
    header->NbIndex--;
    header->FreeSize += delsize;
}

#ifdef LBA2_TARGET_N64
// [hangprof] per-frame accounting of HQR blocs loaded from ROM (reset by the
// frame profiler in N64_BACKEND.CPP).
extern "C" U32 N64ProfNowMs(void);
extern "C" {
U32 g_hqrLoadCount = 0;
U32 g_hqrLoadMs = 0;
U32 g_hqrLoadBytes = 0;
U32 g_hqrEvictCount = 0;
char g_hqrLoadLast[96] = "";
}
#endif

//──────────────────────────────────────────────────────────────────────────
// retourne le pointeur mémoire de la fiche (index) demandée
void *HQR_Get(T_HQR_HEADER *header, S32 index, S32 *outSize) {
    S32 n, oldest;
    U32 testtime;
    void *ptr;
    T_HQR_BLOC *ptrbloc;
    S32 size;
#ifdef LBA2_TARGET_N64
    U32 _hqT0 = 0;
#endif

    if (index < 0)
        goto error;

    ptrbloc = HQR_GiveBloc(index, header->NbIndex, (T_HQR_BLOC *)(header + 1));

    if (ptrbloc) {
        // existing index
        ptrbloc->Time = TimerSystemHR; // update LRU data

        HQR_Flag = FALSE; // NOT NEWLY LOADED

        if (outSize != NULL) {
            *outSize = ptrbloc->Size;
        }

        return ptrbloc->Ptr;
    } else // need load
    {
        // load hqr bloc
#ifdef LBA2_TARGET_N64
        _hqT0 = N64ProfNowMs();
#endif
        size = HQF_Init(header->Name, index);
        if (!size) {
            goto error;
        }

        // memory management
        ptrbloc = (T_HQR_BLOC *)(header + 1);

        // check if enough space for bloc or index
        while ((size > header->FreeSize) OR(header->NbIndex >= header->MaxIndex)) {
            // delete oldest bloc
            oldest = -1;
            testtime = -1;

            for (n = 0; n < header->NbIndex; n++) {
                if (ptrbloc[n].Time < testtime) {
                    testtime = ptrbloc[n].Time;
                    oldest = n;
                }
            }
            if (oldest == -1) // not enough ram or big trouble
            {
                HQF_Close();
            error:
                if (HQRGetErrorFunc)
                    HQRGetErrorFunc(header->Name, index);
                return NULL;
            }

            if (HQRGetDelFunc)
                HQRGetDelFunc(header->Name, index);

            HQR_Del_Bloc(header, oldest);
#ifdef LBA2_TARGET_N64
            g_hqrEvictCount++;
#endif
        }

        // compute ptr
        ptr = (void *)((U8 *)header->Buffer + header->MaxSize - header->FreeSize);

        // space size ok, update struct
        ptrbloc[header->NbIndex].Index = index;
        ptrbloc[header->NbIndex].Time = TimerSystemHR;
        ptrbloc[header->NbIndex].Ptr = ptr;
        ptrbloc[header->NbIndex].Size = size;

        // load it
        if (!HQF_LoadClose(ptr)) {
            goto error;
        }

        header->NbIndex++;
        header->FreeSize -= size;

        HQR_Flag = TRUE; // NEWLY LOADED
#ifdef LBA2_TARGET_N64
        g_hqrLoadCount++;
        g_hqrLoadBytes += (U32)size;
        g_hqrLoadMs += N64ProfNowMs() - _hqT0;
        {
            const char *nm = header->Name;
            for (const char *p = header->Name; *p; p++)
                if (*p == '/' || *p == '\\') nm = p + 1;
            snprintf(g_hqrLoadLast, sizeof(g_hqrLoadLast), "%s#%ld(%ldB)", nm, (long)index, (long)size);
        }
#endif

        if (outSize != NULL) {
            *outSize = size;
        }

        return ptr;
    }
}
