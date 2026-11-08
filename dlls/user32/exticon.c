/*
 *	icon extracting
 *
 * taken and slightly changed from shell
 * this should replace the icon extraction code in shell32 and shell16 once
 * it needs a serious test for compliance with the native API
 *
 * Copyright 2000 Juergen Schmied
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include "user_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(icon);

#pragma pack(push,1)

typedef struct
{
    BYTE        bWidth;          /* Width, in pixels, of the image	*/
    BYTE        bHeight;         /* Height, in pixels, of the image	*/
    BYTE        bColorCount;     /* Number of colors in image (0 if >=8bpp) */
    BYTE        bReserved;       /* Reserved ( must be 0)		*/
    WORD        wPlanes;         /* Color Planes			*/
    WORD        wBitCount;       /* Bits per pixel			*/
    DWORD       dwBytesInRes;    /* How many bytes in this resource?	*/
    DWORD       dwImageOffset;   /* Where in the file is this image?	*/
} icoICONDIRENTRY, *LPicoICONDIRENTRY;

typedef struct
{
    WORD            idReserved;   /* Reserved (must be 0) */
    WORD            idType;       /* Resource Type (RES_ICON or RES_CURSOR) */
    WORD            idCount;      /* How many images */
    icoICONDIRENTRY idEntries[1]; /* An entry for each image (idCount of 'em) */
} icoICONDIR, *LPicoICONDIR;

typedef struct
{
    WORD offset;
    WORD length;
    WORD flags;
    WORD id;
    WORD handle;
    WORD usage;
} NE_NAMEINFO;

typedef struct
{
    WORD  type_id;
    WORD  count;
    DWORD resloader;
} NE_TYPEINFO;

#define NE_RSCTYPE_ICON        0x8003
#define NE_RSCTYPE_GROUP_ICON  0x800e

#pragma pack(pop)

#if 0
static void dumpIcoDirEntry ( LPicoICONDIRENTRY entry )
{
	TRACE("width = 0x%08x height = 0x%08x\n", entry->bWidth, entry->bHeight);
	TRACE("colors = 0x%08x planes = 0x%08x\n", entry->bColorCount, entry->wPlanes);
	TRACE("bitcount = 0x%08x bytesinres = 0x%08lx offset = 0x%08lx\n",
	entry->wBitCount, entry->dwBytesInRes, entry->dwImageOffset);
}
static void dumpIcoDir ( LPicoICONDIR entry )
{
	TRACE("type = 0x%08x count = 0x%08x\n", entry->idType, entry->idCount);
}
#endif

/* A PE file's resources read from the file a structure at a time: the
 * headers, the directories walked and the entries used, never the whole
 * resource section (an installer's can hold a hundred megabytes of payload). */
struct pe_res
{
    HANDLE                 file;
    IMAGE_SECTION_HEADER  *sections;
    UINT                   nsections;
    DWORD                  root;   /* the resource directory's RVA */
    DWORD                  size;   /* and its size */
};

static BOOL pe_res_read_at( HANDLE file, DWORD offset, void *buf, DWORD len )
{
    OVERLAPPED ov = { 0 };
    DWORD got;

    ov.Offset = offset;
    return ReadFile( file, buf, len, &got, &ov ) && got == len;
}

/* LEN bytes at RVA, from the section that holds them */
static BOOL pe_res_read( const struct pe_res *res, DWORD rva, void *buf, DWORD len )
{
    UINT i;

    for (i = 0; i < res->nsections; i++)
    {
        const IMAGE_SECTION_HEADER *sec = res->sections + i;
        if (rva >= sec->VirtualAddress && rva - sec->VirtualAddress < sec->SizeOfRawData)
        {
            if (len > sec->SizeOfRawData - (rva - sec->VirtualAddress)) return FALSE;
            return pe_res_read_at( res->file, sec->PointerToRawData + (rva - sec->VirtualAddress), buf, len );
        }
    }
    return FALSE;
}

static BOOL pe_res_open( HANDLE file, struct pe_res *res )
{
    IMAGE_DOS_HEADER dos;
    union { IMAGE_NT_HEADERS32 nt32; IMAGE_NT_HEADERS64 nt64; } nt;
    const IMAGE_DATA_DIRECTORY *dir;
    OVERLAPPED ov = { 0 };
    DWORD sections, got;

    memset( res, 0, sizeof(*res) );
    res->file = file;
    if (!pe_res_read_at( file, 0, &dos, sizeof(dos) ) || dos.e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    /* the headers, as much of them as the file holds */
    memset( &nt, 0, sizeof(nt) );
    ov.Offset = dos.e_lfanew;
    if (!ReadFile( file, &nt, sizeof(nt), &got, &ov ) || got < offsetof( IMAGE_NT_HEADERS32, OptionalHeader.Magic ) + sizeof(WORD))
        return FALSE;
    if (nt.nt32.Signature != IMAGE_NT_SIGNATURE) return FALSE;
    switch (nt.nt32.OptionalHeader.Magic)
    {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        if (nt.nt32.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return TRUE;
        dir = nt.nt32.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        if (nt.nt64.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return TRUE;
        dir = nt.nt64.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    default:
        return TRUE;  /* a PE, with no resources we can read */
    }
    res->root = dir->VirtualAddress;
    res->size = dir->Size;
    res->nsections = nt.nt32.FileHeader.NumberOfSections;
    sections = dos.e_lfanew + offsetof( IMAGE_NT_HEADERS32, OptionalHeader ) + nt.nt32.FileHeader.SizeOfOptionalHeader;
    if (!(res->sections = malloc( res->nsections * sizeof(*res->sections) ))) res->nsections = 0;
    else if (!pe_res_read_at( file, sections, res->sections, res->nsections * sizeof(*res->sections) ))
        res->nsections = 0;
    return TRUE;
}

/* The directory at OFFSET from the resource root, with its entries (freed by the caller) */
static IMAGE_RESOURCE_DIRECTORY_ENTRY *pe_res_dir( const struct pe_res *res, DWORD offset, IMAGE_RESOURCE_DIRECTORY *dir )
{
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entries;
    DWORD count;

    if (offset >= res->size || !pe_res_read( res, res->root + offset, dir, sizeof(*dir) )) return NULL;
    count = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    if (!(entries = malloc( max( count, 1 ) * sizeof(*entries) ))) return NULL;
    if (count && !pe_res_read( res, res->root + offset + sizeof(*dir), entries, count * sizeof(*entries) ))
    {
        free( entries );
        return NULL;
    }
    return entries;
}

/* The offset of ID's entry under the directory at OFFSET, or -1 */
static DWORD pe_res_find_id( const struct pe_res *res, DWORD offset, WORD id )
{
    IMAGE_RESOURCE_DIRECTORY dir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entries;
    DWORD ret = ~0u;
    int min, max, pos;

    if (!(entries = pe_res_dir( res, offset, &dir ))) return ret;
    min = dir.NumberOfNamedEntries;
    max = min + dir.NumberOfIdEntries - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        if (entries[pos].Id == id) { ret = entries[pos].OffsetToDirectory; break; }
        if (entries[pos].Id > id) max = pos - 1;
        else min = pos + 1;
    }
    free( entries );
    return ret;
}

/* The data of the first language under the directory at OFFSET (freed by the caller) */
static BYTE *pe_res_data( const struct pe_res *res, DWORD offset, DWORD *size )
{
    IMAGE_RESOURCE_DIRECTORY dir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entries;
    IMAGE_RESOURCE_DATA_ENTRY data;
    BYTE *ret;

    if (!(entries = pe_res_dir( res, offset, &dir ))) return NULL;
    offset = dir.NumberOfNamedEntries + dir.NumberOfIdEntries ? entries[0].OffsetToDirectory : ~0u;
    free( entries );
    if (offset >= res->size || !pe_res_read( res, res->root + offset, &data, sizeof(data) )) return NULL;
    /* an icon or a group of them; nothing a shell asks for is this big */
    if (!data.Size || data.Size > 64 * 1024 * 1024 || !(ret = malloc( data.Size ))) return NULL;
    if (!pe_res_read( res, data.OffsetToData, ret, data.Size ))
    {
        free( ret );
        return NULL;
    }
    *size = data.Size;
    return ret;
}

/* ICO_ExtractIconExW for a PE file, from its resources as pe_res reads them */
static UINT ICO_ExtractIconExPE( HANDLE file, HICON *RetPtr, INT nIconIndex, UINT nIcons, UINT cx1, UINT cy1,
                                 UINT cx2, UINT cy2, UINT *pIconId, UINT flags )
{
    IMAGE_RESOURCE_DIRECTORY groupdir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY *groups = NULL;
    struct pe_res res;
    DWORD group, icons, size;
    UINT ret = 0, i, iconDirCount;
    BYTE *data;

    pe_res_open( file, &res );
    if (!res.size)
    {
        WARN("haven't found section for resource directory.\n");
        goto end;
    }
    if ((group = pe_res_find_id( &res, 0, LOWORD(RT_GROUP_ICON) )) == ~0u || !(groups = pe_res_dir( &res, group, &groupdir )))
    {
        WARN("No Icongroupresourcedirectory!\n");
        goto end;
    }
    iconDirCount = groupdir.NumberOfNamedEntries + groupdir.NumberOfIdEntries;

    /* only number of icons requested */
    if (!pIconId)
    {
        ret = iconDirCount;
        goto end;
    }

    if (nIconIndex < 0)
    {
        /* search resource id */
        int iId = abs( nIconIndex );
        for (i = 0; i < iconDirCount; i++)
            if (groups[i].Id == iId) { nIconIndex = i; break; }
        if (nIconIndex < 0)
        {
            WARN("resource id %d not found\n", iId);
            goto end;
        }
    }
    else if (nIconIndex >= iconDirCount)
    {
        WARN("nIconIndex %d is larger than iconDirCount %d\n", nIconIndex, iconDirCount);
        goto end;
    }

    /* assure we don't get too much */
    if (nIcons > iconDirCount - nIconIndex) nIcons = iconDirCount - nIconIndex;

    for (i = 0; i < nIcons; i++)
    {
        if (!(data = pe_res_data( &res, groups[nIconIndex + i].OffsetToDirectory, &size )))
        {
            FIXME("no matching real address for icongroup!\n");
            goto end;
        }
        pIconId[i] = LookupIconIdFromDirectoryEx( data, TRUE, cx1, cy1, flags );
        if (cx2 && cy2) pIconId[++i] = LookupIconIdFromDirectoryEx( data, TRUE, cx2, cy2, flags );
        free( data );
    }

    if ((icons = pe_res_find_id( &res, 0, LOWORD(RT_ICON) )) == ~0u)
    {
        WARN("No Iconresourcedirectory!\n");
        goto end;
    }

    for (i = 0; i < nIcons; i++)
    {
        DWORD icon = pe_res_find_id( &res, icons, LOWORD(pIconId[i]) );

        if (icon == ~0u)
        {
            WARN("icon entry %d not found\n", LOWORD(pIconId[i]));
            RetPtr[i] = 0;
            continue;
        }
        if (!(data = pe_res_data( &res, icon, &size )))
        {
            WARN("no matching real address found for icondata!\n");
            RetPtr[i] = 0;
            continue;
        }
        RetPtr[i] = CreateIconFromResourceEx( data, size, TRUE, 0x00030000, cx1, cy1, flags );
        if (cx2 && cy2) RetPtr[++i] = CreateIconFromResourceEx( data, size, TRUE, 0x00030000, cx2, cy2, flags );
        free( data );
    }
    ret = i;  /* return number of retrieved icons */

end:
    free( groups );
    free( res.sections );
    return ret;
}

/*************************************************************************
 *				USER32_GetResourceTable
 */
static DWORD USER32_GetResourceTable(LPBYTE peimage,DWORD pesize,LPBYTE *retptr)
{
	IMAGE_DOS_HEADER	* mz_header;

	TRACE("%p %p\n", peimage, retptr);

	*retptr = NULL;

	mz_header = (IMAGE_DOS_HEADER*) peimage;

	if (mz_header->e_magic != IMAGE_DOS_SIGNATURE)
	{
	  if (mz_header->e_cblp == 1)	/* .ICO file ? */
	  {
	    *retptr = (LPBYTE)-1;	/* ICONHEADER.idType, must be 1 */
	    return 1;
	  }
	  else
	    return 0; /* failed */
	}
	if (mz_header->e_lfanew >= pesize) {
	    return 0; /* failed, happens with PKZIP DOS Exes for instance. */
	}
	if (*((DWORD*)(peimage + mz_header->e_lfanew)) == IMAGE_NT_SIGNATURE )
	  return IMAGE_NT_SIGNATURE;

	if (*((WORD*)(peimage + mz_header->e_lfanew)) == IMAGE_OS2_SIGNATURE )
	{
	  IMAGE_OS2_HEADER	* ne_header;

	  ne_header = (IMAGE_OS2_HEADER*)(peimage + mz_header->e_lfanew);

	  if (ne_header->ne_magic != IMAGE_OS2_SIGNATURE)
	    return 0;

	  if( (ne_header->ne_restab - ne_header->ne_rsrctab) <= sizeof(NE_TYPEINFO) )
	    *retptr = (LPBYTE)-1;
	  else
	    *retptr = peimage + mz_header->e_lfanew + ne_header->ne_rsrctab;

	  return IMAGE_OS2_SIGNATURE;
	}
	return 0; /* failed */
}
/*************************************************************************
 *			USER32_LoadResource
 */
static BYTE * USER32_LoadResource( LPBYTE peimage, NE_NAMEINFO* pNInfo, WORD sizeShift, ULONG *uSize)
{
	TRACE("%p %p 0x%08x\n", peimage, pNInfo, sizeShift);

	*uSize = (DWORD)pNInfo->length << sizeShift;
	return peimage + ((DWORD)pNInfo->offset << sizeShift);
}

/*************************************************************************
 *                      ICO_LoadIcon
 */
static BYTE * ICO_LoadIcon( LPBYTE peimage, LPicoICONDIRENTRY lpiIDE, ULONG *uSize)
{
	TRACE("%p %p\n", peimage, lpiIDE);

	*uSize = lpiIDE->dwBytesInRes;
	return peimage + lpiIDE->dwImageOffset;
}

/*************************************************************************
 *                      ICO_GetIconDirectory
 *
 * Reads .ico file and build phony ICONDIR struct
 */
static BYTE * ICO_GetIconDirectory( LPBYTE peimage, LPicoICONDIR* lplpiID, ULONG *uSize )
{
	CURSORICONFILEDIR *lpcid;	/* icon resource in resource-dir format */
	CURSORICONDIR	* lpID;		/* icon resource in resource format */
	int		i;

	TRACE("%p %p\n", peimage, lplpiID);

	lpcid = (CURSORICONFILEDIR*)peimage;

	if( lpcid->idReserved || (lpcid->idType != 1) || (!lpcid->idCount) )
	  return 0;

	/* allocate the phony ICONDIR structure */
        *uSize = FIELD_OFFSET(CURSORICONDIR, idEntries[lpcid->idCount]);
	if( (lpID = HeapAlloc(GetProcessHeap(),0, *uSize) ))
	{
	  /* copy the header */
	  lpID->idReserved = lpcid->idReserved;
	  lpID->idType = lpcid->idType;
	  lpID->idCount = lpcid->idCount;

	  /* copy the entries */
	  for( i=0; i < lpcid->idCount; i++ )
	  {
            memcpy(&lpID->idEntries[i], &lpcid->idEntries[i], sizeof(CURSORICONDIRENTRY) - 2);
	    lpID->idEntries[i].wResId = i;
	  }

	  *lplpiID = (LPicoICONDIR)peimage;
	  return (BYTE *)lpID;
	}
	return 0;
}

/*************************************************************************
 *	ICO_ExtractIconExW		[internal]
 *
 * NOTES
 *  nIcons = 0: returns number of Icons in file
 *
 * returns
 *  invalid file: -1
 *  failure:0;
 *  success: number of icons in file (nIcons = 0) or nr of icons retrieved
 */
static UINT ICO_ExtractIconExW(
	LPCWSTR lpszExeFileName,
	HICON * RetPtr,
	INT nIconIndex,
	UINT nIcons,
	UINT cxDesired,
	UINT cyDesired,
	UINT *pIconId,
	UINT flags)
{
	UINT		ret = 0;
	UINT		cx1, cx2, cy1, cy2;
	LPBYTE		pData;
	DWORD		sig;
	HANDLE		hFile;
	UINT16		iconDirCount = 0,iconCount = 0;
	LPBYTE		peimage;
	HANDLE		fmapping;
	DWORD		fsizeh,fsizel;
        WCHAR		szExePath[MAX_PATH];
        DWORD		dwSearchReturn;

	TRACE("%s, %d, %d %p 0x%08x\n", debugstr_w(lpszExeFileName), nIconIndex, nIcons, pIconId, flags);

        dwSearchReturn = SearchPathW(NULL, lpszExeFileName, NULL, ARRAY_SIZE(szExePath), szExePath, NULL);
        if ((dwSearchReturn == 0) || (dwSearchReturn > ARRAY_SIZE(szExePath)))
        {
            WARN("File %s not found or path too long\n", debugstr_w(lpszExeFileName));
            return -1;
        }

	hFile = CreateFileW(szExePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0);
	if (hFile == INVALID_HANDLE_VALUE) return ret;

	/* a PE's icons are read from it, not mapped whole */
	{
	    struct pe_res res;
	    BOOL pe = pe_res_open(hFile, &res);
	    free(res.sections);
	    if (pe)
	    {
	        if (pIconId) *pIconId = 0xFFFFFFFF;
	        ret = ICO_ExtractIconExPE(hFile, RetPtr, nIconIndex, nIcons, LOWORD(cxDesired), LOWORD(cyDesired),
	                                  HIWORD(cxDesired), HIWORD(cyDesired), pIconId ? pIconId : (UINT *)RetPtr, flags);
	        CloseHandle(hFile);
	        return ret;
	    }
	}
	fsizel = GetFileSize(hFile,&fsizeh);

	/* Map the file */
	fmapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY | SEC_COMMIT, 0, 0, NULL);
	CloseHandle(hFile);
	if (!fmapping)
	{
          WARN("CreateFileMapping error %ld\n", GetLastError() );
	  return 0xFFFFFFFF;
	}

	if (!(peimage = MapViewOfFile(fmapping, FILE_MAP_READ, 0, 0, 0)))
	{
          WARN("MapViewOfFile error %ld\n", GetLastError() );
	  CloseHandle(fmapping);
	  return 0xFFFFFFFF;
	}
	CloseHandle(fmapping);

	cx1 = LOWORD(cxDesired);
	cx2 = HIWORD(cxDesired);
	cy1 = LOWORD(cyDesired);
	cy2 = HIWORD(cyDesired);

	if (pIconId) /* Invalidate first icon identifier */
		*pIconId = 0xFFFFFFFF;

	if (!pIconId) /* if no icon identifier array present use the icon handle array as intermediate storage */
	  pIconId = (UINT*)RetPtr;

	sig = USER32_GetResourceTable(peimage, fsizel, &pData);

/* ico file or NE exe/dll*/
	if (sig==IMAGE_OS2_SIGNATURE || sig==1) /* .ICO file */
	{
	  BYTE		*pCIDir = 0;
	  NE_TYPEINFO	*pTInfo = (NE_TYPEINFO*)(pData + 2);
	  NE_NAMEINFO	*pIconStorage = NULL;
	  NE_NAMEINFO	*pIconDir = NULL;
	  LPicoICONDIR	lpiID = NULL;
	  ULONG		uSize = 0;

          TRACE("-- OS2/icon Signature (0x%08lx)\n", sig);

	  if (pData == (BYTE*)-1)
	  {
	    pCIDir = ICO_GetIconDirectory(peimage, &lpiID, &uSize);	/* check for .ICO file */
	    if (pCIDir)
	    {
	      iconDirCount = 1; iconCount = lpiID->idCount;
              TRACE("-- icon found %p 0x%08lx 0x%08x 0x%08x\n", pCIDir, uSize, iconDirCount, iconCount);
	    }
	  }
	  else while (pTInfo->type_id && !(pIconStorage && pIconDir))
	  {
	    if (pTInfo->type_id == NE_RSCTYPE_GROUP_ICON)	/* find icon directory and icon repository */
	    {
	      iconDirCount = pTInfo->count;
	      pIconDir = ((NE_NAMEINFO*)(pTInfo + 1));
	      TRACE("\tfound directory - %i icon families\n", iconDirCount);
	    }
	    if (pTInfo->type_id == NE_RSCTYPE_ICON)
	    {
	      iconCount = pTInfo->count;
	      pIconStorage = ((NE_NAMEINFO*)(pTInfo + 1));
	      TRACE("\ttotal icons - %i\n", iconCount);
	    }
	    pTInfo = (NE_TYPEINFO *)((char*)(pTInfo+1)+pTInfo->count*sizeof(NE_NAMEINFO));
	  }

	  if ((pIconStorage && pIconDir) || lpiID)	  /* load resources and create icons */
	  {
	    if (nIcons == 0)
	    {
	      ret = iconDirCount;
              if (lpiID)	/* *.ico file, deallocate heap pointer*/
	        HeapFree(GetProcessHeap(), 0, pCIDir);
	    }
	    else if (nIconIndex < iconDirCount)
	    {
	      UINT16   i, icon;
	      if (nIcons > iconDirCount - nIconIndex)
	        nIcons = iconDirCount - nIconIndex;

	      for (i = 0; i < nIcons; i++)
	      {
	        /* .ICO files have only one icon directory */
	        if (lpiID == NULL)	/* not *.ico */
	          pCIDir = USER32_LoadResource(peimage, pIconDir + i + nIconIndex, *(WORD*)pData, &uSize);
	        pIconId[i] = LookupIconIdFromDirectoryEx(pCIDir, TRUE, cx1, cy1, flags);
                if (cx2 && cy2) pIconId[++i] = LookupIconIdFromDirectoryEx(pCIDir, TRUE,  cx2, cy2, flags);
	      }
              if (lpiID)	/* *.ico file, deallocate heap pointer*/
	        HeapFree(GetProcessHeap(), 0, pCIDir);

	      for (icon = 0; icon < nIcons; icon++)
	      {
	        pCIDir = NULL;
	        if (lpiID)
	          pCIDir = ICO_LoadIcon(peimage, lpiID->idEntries + (int)pIconId[icon], &uSize);
	        else
	          for (i = 0; i < iconCount; i++)
	            if (pIconStorage[i].id == ((int)pIconId[icon] | 0x8000) )
	              pCIDir = USER32_LoadResource(peimage, pIconStorage + i, *(WORD*)pData, &uSize);

	        if (pCIDir)
                {
	          RetPtr[icon] = CreateIconFromResourceEx(pCIDir, uSize, TRUE, 0x00030000,
                                                                 cx1, cy1, flags);
                  if (cx2 && cy2)
                      RetPtr[++icon] = CreateIconFromResourceEx(pCIDir, uSize, TRUE, 0x00030000,
                                                                       cx2, cy2, flags);
                }
	        else
	          RetPtr[icon] = 0;
	      }
	      ret = icon;	/* return number of retrieved icons */
	    }
	  }
	}
/* end ico file */

	UnmapViewOfFile(peimage);	/* success */
	return ret;
}

/***********************************************************************
 *           PrivateExtractIconsW			[USER32.@]
 *
 * NOTES
 *  If HIWORD(sizeX) && HIWORD(sizeY) 2 * ((nIcons + 1) MOD 2) icons are
 *  returned, with the LOWORD size icon first and the HIWORD size icon
 *  second.
 *  Also the Windows equivalent does extract icons in a strange way if
 *  nIndex is negative. Our implementation treats a negative nIndex as
 *  looking for that resource identifier for the first icon to retrieve.
 *
 * FIXME:
 *  should also support 16 bit EXE + DLLs, cursor and animated cursor as
 *  well as bitmap files.
 */

UINT WINAPI PrivateExtractIconsW (
	LPCWSTR lpwstrFile,
	int nIndex,
	int sizeX,
	int sizeY,
	HICON * phicon, /* [out] pointer to array of nIcons HICON handles */
	UINT* pIconId,  /* [out] pointer to array of nIcons icon identifiers or NULL */
	UINT nIcons,    /* [in] number of icons to retrieve */
	UINT flags )    /* [in] LR_* flags used by LoadImage */
{
	TRACE("%s %d %dx%d %p %p %d 0x%08x\n",
	      debugstr_w(lpwstrFile), nIndex, sizeX, sizeY, phicon, pIconId, nIcons, flags);

	if ((nIcons & 1) && HIWORD(sizeX) && HIWORD(sizeY))
	{
	  WARN("Uneven number %d of icons requested for small and large icons!\n", nIcons);
	}
	return ICO_ExtractIconExW(lpwstrFile, phicon, nIndex, nIcons, sizeX, sizeY, pIconId, flags);
}

/***********************************************************************
 *           PrivateExtractIconsA			[USER32.@]
 */

UINT WINAPI PrivateExtractIconsA (
	LPCSTR lpstrFile,
	int nIndex,
	int sizeX,
	int sizeY,
	HICON * phicon, /* [out] pointer to array of nIcons HICON handles */
	UINT* piconid,  /* [out] pointer to array of nIcons icon identifiers or NULL */
	UINT nIcons,    /* [in] number of icons to retrieve */
	UINT flags )    /* [in] LR_* flags used by LoadImage */
{
    UINT ret;
    INT len = MultiByteToWideChar(CP_ACP, 0, lpstrFile, -1, NULL, 0);
    LPWSTR lpwstrFile = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

    MultiByteToWideChar(CP_ACP, 0, lpstrFile, -1, lpwstrFile, len);
    ret = PrivateExtractIconsW(lpwstrFile, nIndex, sizeX, sizeY, phicon, piconid, nIcons, flags);

    HeapFree(GetProcessHeap(), 0, lpwstrFile);
    return ret;
}

/***********************************************************************
 *           PrivateExtractIconExW			[USER32.@]
 * NOTES
 *  if nIndex == -1 it returns the number of icons in any case !!!
 */
UINT WINAPI PrivateExtractIconExW (
	LPCWSTR lpwstrFile,
	int nIndex,
	HICON * phIconLarge,
	HICON * phIconSmall,
	UINT nIcons )
{
	DWORD cyicon, cysmicon, cxicon, cxsmicon;
	UINT ret = 0;

	TRACE("%s %d %p %p %d\n",
	debugstr_w(lpwstrFile),nIndex,phIconLarge, phIconSmall, nIcons);

	if (nIndex == -1)
	  /* get the number of icons */
	  return ICO_ExtractIconExW(lpwstrFile, NULL, 0, 0, 0, 0, NULL, LR_DEFAULTCOLOR);

	if (nIcons == 1 && phIconSmall && phIconLarge)
	{
	  HICON hIcon[2];
	  cxicon = GetSystemMetrics(SM_CXICON);
	  cyicon = GetSystemMetrics(SM_CYICON);
	  cxsmicon = GetSystemMetrics(SM_CXSMICON);
	  cysmicon = GetSystemMetrics(SM_CYSMICON);

          ret = ICO_ExtractIconExW(lpwstrFile, hIcon, nIndex, 2, cxicon | (cxsmicon<<16),
	                           cyicon | (cysmicon<<16), NULL, LR_DEFAULTCOLOR);
	  *phIconLarge = hIcon[0];
	  *phIconSmall = hIcon[1];
 	  return ret;
	}

	if (phIconSmall)
	{
	  /* extract n small icons */
	  cxsmicon = GetSystemMetrics(SM_CXSMICON);
	  cysmicon = GetSystemMetrics(SM_CYSMICON);
	  ret = ICO_ExtractIconExW(lpwstrFile, phIconSmall, nIndex, nIcons, cxsmicon,
	                           cysmicon, NULL, LR_DEFAULTCOLOR);
	}
       if (phIconLarge)
	{
	  /* extract n large icons */
	  cxicon = GetSystemMetrics(SM_CXICON);
	  cyicon = GetSystemMetrics(SM_CYICON);
         ret = ICO_ExtractIconExW(lpwstrFile, phIconLarge, nIndex, nIcons, cxicon,
	                           cyicon, NULL, LR_DEFAULTCOLOR);
	}
	return ret;
}

/***********************************************************************
 *           PrivateExtractIconExA			[USER32.@]
 */
UINT WINAPI PrivateExtractIconExA (
	LPCSTR lpstrFile,
	int nIndex,
	HICON * phIconLarge,
	HICON * phIconSmall,
	UINT nIcons )
{
	UINT ret;
	INT len = MultiByteToWideChar(CP_ACP, 0, lpstrFile, -1, NULL, 0);
	LPWSTR lpwstrFile = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

	TRACE("%s %d %p %p %d\n", lpstrFile, nIndex, phIconLarge, phIconSmall, nIcons);

	MultiByteToWideChar(CP_ACP, 0, lpstrFile, -1, lpwstrFile, len);
	ret = PrivateExtractIconExW(lpwstrFile, nIndex, phIconLarge, phIconSmall, nIcons);
	HeapFree(GetProcessHeap(), 0, lpwstrFile);
	return ret;
}
