/******************************************************************************
*
* Copyright (C) 2019-2020 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*
*
*
******************************************************************************/


#include "xpm_common.h"
#include "xpm_node.h"
#include "xpm_regs.h"
#include "xpm_subsystem.h"
#include "xpm_device.h"
#include "xpm_mem.h"
#include "xpm_prot.h"

static XPm_Prot *PmProtNodes[XPM_NODEIDX_PROT_MAX];

#define SIZE_64K				(0x10000U)
#define SIZE_1M					(0x100000U)
#define SIZE_512M				(0x20000000U)

/**
 * Following macros represent staring and ending aperture offsets
 * as defined in the XPPU HW Spec.
 * 	64k Apertures: 0-255
 * 	1m Apertures: 384-399
 * 	512m Aperture: 400
 */
#define APER_64K_START				(0U)
#define APER_64K_END				(255U)
#define APER_1M_START				(384U)
#define APER_1M_END				(399U)
#define APER_512M_START				(400U)
#define APER_512M_END				(400U)

#define MAX_PERM_REGS				(13U)
#define MAX_APER_PARITY_FIELDS			(4U)
#define APER_PARITY_FIELD_WIDTH			(5U)
#define APER_PARITY_FIELD_MASK			(0x1FU)

/** Refer: Section "XPPU protection for IPI" from XPPU Spec */
#define APER_IPI_MIN				(49U)
#define APER_IPI_MAX				(63U)

/**
 * Max XMPU regions count
 */
#define MAX_MEM_REGIONS				(16U)

/**
 * Memory region type to XMPU map.
 */
#define MEMIDX(Idx) \
	((Idx) - XPM_NODETYPE_DEV_OCM_REGN)

static const u8 MemToXMPUMap[2] = {
	[MEMIDX(XPM_NODETYPE_DEV_OCM_REGN)] = XPM_NODEIDX_PROT_XMPU_OCM,
	[MEMIDX(XPM_NODETYPE_DEV_DDR_REGN)] = XPM_NODEIDX_PROT_XMPU_PMC,
};

/**
 * Get relevant XMPU instance index from corresponding memory region type.
 */
#define MEM_TYPE_TO_XMPU(MemRegnNodeId)	\
	(MemToXMPUMap[MEMIDX(NODETYPE(MemRegnNodeId))])

/**
 * For XMPU Region-12 (0-based):
 *   Offset step size is different
 */
static const u8 RegnCfgOffsets[MAX_MEM_REGIONS] = {
	0x00, 0x18, 0x18, 0x18,
	0x18, 0x18, 0x18, 0x18,
	0x18, 0x18, 0x18, 0x18,
	0x20, 0x18, 0x18, 0x18,
};

/**
 * For XMPU Region-13 (0-based):
 *   Offset step size is different
 */
static const u8 RegnAddrOffsets[MAX_MEM_REGIONS] = {
	0x00, 0x18, 0x18, 0x18,
	0x18, 0x18, 0x18, 0x18,
	0x18, 0x18, 0x18, 0x18,
	0x18, 0x20, 0x18, 0x18,
};

/**
 * Returns XMPU Region "config" offset for given region number.
 */
#define REGN_CONFIG_OFFSET(region)	((u32)RegnCfgOffsets[(region)])

/**
 * Returns XMPU Region "address" offset for given region number.
 * For:
 *  - Start Lo/Hi addr registers
 *  - End lo/hi addr registers
 */
#define REGN_ADDR_OFFSET(region)	((u32)RegnAddrOffsets[(region)])


/****************************************************************************/
/**
 * @brief  Initialize protection base class struct and add it to database
 *
 * @param  ProtNode: Pointer to an uninitialized protection class struct
 * @param  Id: Node Id assigned to a XPPU/XMPU node
 * @param  BaseAddr: Base address of the given XPPU/XMPU
 *
 * @return XST_SUCCESS if successful; appropriate failure code otherwise
 *
 * @note   This is internally called from XMPU/XPPU initialization routines
 *
 ****************************************************************************/
static XStatus XPmProt_Init(XPm_Prot *ProtNode, u32 Id, u32 BaseAddr)
{
	XStatus Status = XST_FAILURE;
	u32 NodeIndex = NODEINDEX(Id);

	if ((u32)XPM_NODEIDX_PROT_MAX <= NodeIndex) {
		goto done;
	}

	XPmNode_Init(&ProtNode->Node, Id, (u8)XPM_PROT_DISABLED, BaseAddr);

	PmProtNodes[NodeIndex] = ProtNode;
	Status = XST_SUCCESS;

done:
	return Status;
}

XStatus XPmProtPpu_Init(XPm_ProtPpu *PpuNode, u32 Id, u32 BaseAddr)
{
	XStatus Status = XST_FAILURE;

	Status = XPmProt_Init((XPm_Prot *)PpuNode, Id, BaseAddr);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	/* Parity status bits */
	PpuNode->MIDParityEn = 0;
	PpuNode->AperParityEn = 0;

	/* Init addresses - 64k */
	PpuNode->Aperture_64k.NumSupported = 0;
	PpuNode->Aperture_64k.StartAddress = 0;
	PpuNode->Aperture_64k.EndAddress = 0;

	/* Init addresses - 1m */
	PpuNode->Aperture_1m.NumSupported = 0;
	PpuNode->Aperture_1m.StartAddress = 0;
	PpuNode->Aperture_1m.EndAddress = 0;

	/* Init addresses - 512mb */
	PpuNode->Aperture_512m.NumSupported = 0;
	PpuNode->Aperture_512m.StartAddress = 0;
	PpuNode->Aperture_512m.EndAddress = 0;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Initialize and add a XMPU instance to database
 *
 * @param  MpuNode: Pointer to an uninitialized XMPU data structure
 * @param  Id: Node Id assigned to a XMPU node
 * @param  BaseAddr: Base address of the given XMPU
 *
 * @return XST_SUCCESS if successful; appropriate failure code otherwise
 *
 ****************************************************************************/
XStatus XPmProtMpu_Init(XPm_ProtMpu *MpuNode, u32 Id, u32 BaseAddr)
{
	XStatus Status = XST_FAILURE;

	Status = XPmProt_Init((XPm_Prot *)MpuNode, Id, BaseAddr);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	/* Init */
	MpuNode->AlignCfg = 0;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Returns a handle to protection node from given Node Id
 *
 * @param  Id: Node Id assigned to a XPPU/XMPU node
 *
 * @return A handle to given node; else NULL
 *
 ****************************************************************************/
static XPm_Prot *XPmProt_GetById(const u32 Id)
{
	XPm_Prot *ProtNode = NULL;

	if (((u32)XPM_NODECLASS_PROTECTION != NODECLASS(Id)) ||
	    ((u32)XPM_NODEIDX_PROT_MAX <= NODEINDEX(Id))) {
		goto done;
	}

	ProtNode = PmProtNodes[NODEINDEX(Id)];
	/* Check that internal ID is same as given ID or not. */
	if ((NULL != ProtNode) && (Id != ProtNode->Node.Id)) {
		ProtNode = NULL;
	}

done:
	return ProtNode;
}

/****************************************************************************/
/**
 * @brief  Returns a handle to protection node from given Node Id/Index
 *
 * @param  Idx: Node Id/Index assigned to a XPPU/XMPU node
 *
 * @return A handle to given node; else NULL
 *
 ****************************************************************************/
static XPm_Prot *XPmProt_GetByIndex(const u32 Idx)
{
	XPm_Prot *ProtNode = NULL;

	if ((u32)XPM_NODEIDX_PROT_MAX <= NODEINDEX(Idx)) {
		goto done;
	}

	ProtNode = PmProtNodes[NODEINDEX(Idx)];
	/* Check that internal Index is same as given Index or not. */
	if ((NULL != ProtNode) && (Idx != NODEINDEX(ProtNode->Node.Id))) {
		ProtNode = NULL;
	}

done:
	return ProtNode;
}

static void XPmProt_XppuSetAperture(const XPm_ProtPpu *PpuNode, u32 AperAddr, u32 AperVal)
{
	u32 i, RegVal, Field, FieldParity, Tz;
	u32 Parity = 0;

	/* Clear parity bits */
	RegVal = AperVal & ~XPPU_APERTURE_PARITY_MASK;

	if (0x0U == PpuNode->AperParityEn) {
		goto done;
	}

	/* Extract TrustZone bit */
	Tz = (RegVal >> XPPU_APERTURE_TRUSTZONE_OFFSET) & 0x1U;

	/**
	 * Enabling aperture parity provides a benefit that in terms of
	 * security of the XPPU module itself. Anytime an aperture entry
	 * is fetched from the local RAM, HW and SW computed parities are
	 * compared; if they mismatch then transaction fails and parity error
	 * is flagged.
	 *
	 * Parity for bits in this register.
	 * bit 28: [4:0].
	 * bit 29: [9:5].
	 * bit 30: [14:10].
	 * bit 31: [27],[19:15].
	 */
	for (i = 0; i < MAX_APER_PARITY_FIELDS; i++)
	{
		Field = (RegVal >> (i * APER_PARITY_FIELD_WIDTH));
		FieldParity = XPm_ComputeParity(Field & APER_PARITY_FIELD_MASK);
		if (i == MAX_APER_PARITY_FIELDS - 1U) {
			FieldParity ^= Tz;
		}
		Parity |= (FieldParity << i);
	}

	/* Set parity bits */
	RegVal |= (Parity << XPPU_APERTURE_PARITY_SHIFT);

done:
	PmOut32(AperAddr, RegVal);
}

static XStatus XPmProt_XppuEnable(u32 NodeId, u32 ApertureInitVal)
{
	XStatus Status = XST_FAILURE;
	u32 i = 0;
	XPm_ProtPpu *PpuNode = (XPm_ProtPpu *)XPmProt_GetById(NodeId);
	u32 Address, BaseAddr, RegVal;

	if (PpuNode == NULL) {
		goto done;
	}

	/* XPPU Base Address */
	BaseAddr = PpuNode->ProtNode.Node.BaseAddress;

	if ((PLATFORM_VERSION_SILICON == Platform) && (PLATFORM_VERSION_SILICON_ES1 == PlatformVersion)) {
		/* Disable permission checks for all apertures */
		Address = BaseAddr + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET;
		for (i = 0; i < MAX_PERM_REGS; i++)
		{
			PmOut32(Address, 0x0U);
			Address = Address + 0x4U;
		}
	}

	/* Get Number of Apertures supported */
	PmIn32(BaseAddr + XPPU_M_APERTURE_64KB_OFFSET, RegVal);
	PpuNode->Aperture_64k.NumSupported = RegVal;
	Xil_AssertNonvoid((APER_64K_END - APER_64K_START + 1U) == PpuNode->Aperture_64k.NumSupported);

	PmIn32(BaseAddr + XPPU_M_APERTURE_1MB_OFFSET, RegVal);
	PpuNode->Aperture_1m.NumSupported = RegVal;
	Xil_AssertNonvoid((APER_1M_END - APER_1M_START + 1U) == PpuNode->Aperture_1m.NumSupported);

	PmIn32(BaseAddr + XPPU_M_APERTURE_512MB_OFFSET, RegVal);
	PpuNode->Aperture_512m.NumSupported = RegVal;
	Xil_AssertNonvoid((APER_512M_END - APER_512M_START + 1U) == PpuNode->Aperture_512m.NumSupported);

	/* Store parity bits settings */
	PmIn32(BaseAddr + XPPU_CTRL_OFFSET, RegVal);
	PpuNode->MIDParityEn = (u8)((RegVal >> XPPU_CTRL_MID_PARITY_EN_SHIFT) & 0x1U);
	PpuNode->AperParityEn = (u8)((RegVal >> XPPU_CTRL_APER_PARITY_EN_SHIFT) & 0x1U);

	/* Initialize all apertures for default value */
	Address = BaseAddr + XPPU_APERTURE_0_OFFSET;
	for (i = APER_64K_START; i <= APER_64K_END; i++) {
		/**
		 * In Versal, message buffer protection is moved out of XPPU, to IPI.
		 * Therefore, XPPU permissions for IPI specific apertures (aperture 49 to aperture 63)
		 * should be configured to allow all. This is applicable to LPD XPPU only.
		 *
		 * Refer "XPPU protection for IPI" from XPPU Spec
		 */
		if (((u32)XPM_NODEIDX_PROT_XPPU_LPD == NODEINDEX(NodeId))
			&& ((i >= APER_IPI_MIN) && (i <= APER_IPI_MAX))) {
			XPmProt_XppuSetAperture(PpuNode, Address, (ApertureInitVal | XPPU_APERTURE_PERMISSION_MASK));
		} else {
			XPmProt_XppuSetAperture(PpuNode, Address, ApertureInitVal);
		}
		Address = Address + 0x4U;
	}
	Address = BaseAddr + XPPU_APERTURE_384_OFFSET;
	for (i = APER_1M_START; i <= APER_1M_END; i++) {
		XPmProt_XppuSetAperture(PpuNode, Address, ApertureInitVal);
		Address = Address + 0x4U;
	}
	Address = BaseAddr + XPPU_APERTURE_400_OFFSET;
	for (i = APER_512M_START; i <= APER_512M_END; i++) {
		XPmProt_XppuSetAperture(PpuNode, Address, ApertureInitVal);
		Address = Address + 0x4U;
	}

	if ((PLATFORM_VERSION_SILICON == Platform) && (PLATFORM_VERSION_SILICON_ES1 == PlatformVersion)) {
		/* Enable permission checks for all apertures */
		Address = BaseAddr + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET;
		for (i = 0; i < MAX_PERM_REGS; i++)
		{
			PmOut32(Address, 0xFFFFFFFFU);
			Address = Address + 0x4U;
		}
	}

	/* Get Aperture start and end addresses */
	PmIn32(BaseAddr + XPPU_BASE_64KB_OFFSET, RegVal);
	PpuNode->Aperture_64k.StartAddress = RegVal;
	PpuNode->Aperture_64k.EndAddress =
		(PpuNode->Aperture_64k.StartAddress + (PpuNode->Aperture_64k.NumSupported * SIZE_64K) - 1U);

	PmIn32(BaseAddr + XPPU_BASE_1MB_OFFSET, RegVal);
	PpuNode->Aperture_1m.StartAddress = RegVal;
	PpuNode->Aperture_1m.EndAddress =
		(PpuNode->Aperture_1m.StartAddress + (PpuNode->Aperture_1m.NumSupported * SIZE_1M) - 1U);

	PmIn32(BaseAddr + XPPU_BASE_512MB_OFFSET, RegVal);
	PpuNode->Aperture_512m.StartAddress = RegVal;
	PpuNode->Aperture_512m.EndAddress =
		(PpuNode->Aperture_512m.StartAddress + (PpuNode->Aperture_512m.NumSupported * SIZE_512M) - 1U);

	/* Enable Xppu */
	PmRmw32(BaseAddr + XPPU_CTRL_OFFSET, XPPU_CTRL_ENABLE_MASK, XPPU_CTRL_ENABLE_MASK);

	PpuNode->ProtNode.Node.State = (u8)XPM_PROT_ENABLED;

	Status = XST_SUCCESS;

done:
	return Status;
}

static XStatus XPmProt_XppuDisable(u32 NodeId)
{
	XStatus Status = XST_FAILURE;
	XPm_ProtPpu *PpuNode = (XPm_ProtPpu *)XPmProt_GetById(NodeId);
	u32 Address, idx;

	if (PpuNode == NULL) {
		Status = XST_FAILURE;
		goto done;
	}

	if ((PLATFORM_VERSION_SILICON == Platform) && (PLATFORM_VERSION_SILICON_ES1 == PlatformVersion)) {
		/* Disable permission checks for all apertures */
		Address = PpuNode->ProtNode.Node.BaseAddress + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET;
		for (idx = 0; idx < MAX_PERM_REGS; idx++)
		{
			PmOut32(Address, 0x0);
			Address = Address + 0x4U;
		}
	}

	/* Disable Xppu */
	PmRmw32(PpuNode->ProtNode.Node.BaseAddress + XPPU_CTRL_OFFSET,
			XPPU_CTRL_ENABLE_MASK, ~XPPU_CTRL_ENABLE_MASK);

	PpuNode->ProtNode.Node.State = (u8)XPM_PROT_DISABLED;

	Status = XST_SUCCESS;

done:
	return Status;
}

static XStatus XPmProt_XppuConfigure(const XPm_Requirement *Reqm, u32 Enable)
{
	XStatus Status = XST_FAILURE;
	u32 DeviceBaseAddr = Reqm->Device->Node.BaseAddress;
	XPm_ProtPpu *PpuNode = NULL;
	u32 ApertureOffset = 0, ApertureAddress = 0;
	u32 Permissions = 0, i;
	u32 DynamicReconfigAddrOffset = 0;
	u32	PermissionRegAddress = 0;
	u32	PermissionRegMask = 0;

        PmDbg("Xppu configure %x\r\n", Enable);
	PmDbg("Device base %x\r\n", DeviceBaseAddr);

	/* Find XPPU */
	for (i = 0; i < (u32)XPM_NODEIDX_PROT_MAX; i++)
	{
		if ((PmProtNodes[i] != NULL)
		&& ((u32)XPM_NODESUBCL_PROT_XPPU == NODESUBCLASS(PmProtNodes[i]->Node.Id))) {
			PpuNode = (XPm_ProtPpu *)PmProtNodes[i];
			if ((DeviceBaseAddr >= PpuNode->Aperture_64k.StartAddress) && (DeviceBaseAddr <= PpuNode->Aperture_64k.EndAddress)) {
				ApertureOffset =  (DeviceBaseAddr - PpuNode->Aperture_64k.StartAddress) / SIZE_64K;
				ApertureAddress = (PpuNode->ProtNode.Node.BaseAddress + XPPU_APERTURE_0_OFFSET) + (ApertureOffset * 4U);
				DynamicReconfigAddrOffset = ApertureOffset;
				PermissionRegAddress = PpuNode->ProtNode.Node.BaseAddress + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET + (((APER_64K_START + ApertureOffset) / 32U) * 4U);
				PermissionRegMask = (u32)1U << ((APER_64K_START + ApertureOffset) % 32U);
			} else if ((DeviceBaseAddr >= PpuNode->Aperture_1m.StartAddress) && (DeviceBaseAddr <= PpuNode->Aperture_1m.EndAddress)) {
				ApertureOffset =  (DeviceBaseAddr - PpuNode->Aperture_1m.StartAddress) / SIZE_1M;
				ApertureAddress = (PpuNode->ProtNode.Node.BaseAddress + XPPU_APERTURE_384_OFFSET) + (ApertureOffset * 4U);
				DynamicReconfigAddrOffset = ApertureOffset;
				PermissionRegAddress = PpuNode->ProtNode.Node.BaseAddress + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET + (((APER_1M_START + ApertureOffset) / 32U) * 4U);
				PermissionRegMask = (u32)1U << ((APER_1M_START + ApertureOffset) % 32U);
			/*TODO: 512M start and end address need to be validated */
			/*} else if ((DeviceBaseAddr >= PpuNode->Aperture_512m.StartAddress) && (DeviceBaseAddr <= PpuNode->Aperture_512m.EndAddress)) {
				ApertureOffset =  (DeviceBaseAddr - PpuNode->Aperture_512m.StartAddress) / SIZE_512M;
				ApertureAddress = (PpuNode->ProtNode.Node.BaseAddress + XPPU_APERTURE_400_OFFSET) + (ApertureOffset * 4);
				DynamicReconfigAddrOffset = APER_512M_START;
				PermissionRegAddress = PpuNode->ProtNode.Node.BaseAddress + XPPU_ENABLE_PERM_CHECK_REG00_OFFSET + (((APER_512M_START + ApertureOffset) / 32) * 4);
				PermissionRegMask = 1 << ((APER_512M_START + ApertureOffset) % 32); */
			} else {
				continue;
			}
			break;
		}
	}
	if ((i == (u32)XPM_NODEIDX_PROT_MAX) || (NULL == PpuNode) || (0U == ApertureAddress)) {
		Status = XST_SUCCESS;
		goto done;
	}

	/* See if XPPU is enabled or not, if not, return */
	if ((u8)XPM_PROT_DISABLED == PpuNode->ProtNode.Node.State) {
		Status = XST_SUCCESS;
		goto done;
	}

	PmDbg("Aperoffset %x AperAddress %x DynamicReconfigAddrOffset %x\r\n",ApertureOffset, ApertureAddress, DynamicReconfigAddrOffset);

	if (0U != Enable) {
		u8 UsagePolicy = USAGE_POLICY(Reqm->Flags);
		u32 Security = SECURITY_POLICY(Reqm->Flags);

		PmIn32(ApertureAddress, Permissions);

		/* Configure XPPU Aperture */
		if ((UsagePolicy == (u8)REQ_NONSHARED) || (UsagePolicy == (u8)REQ_TIME_SHARED)) {
			Permissions = ((Security << XPPU_APERTURE_TRUSTZONE_OFFSET) | (Reqm->Params[0] & XPPU_APERTURE_PERMISSION_MASK));
		} else if (UsagePolicy == (u8)REQ_SHARED) {
			/* if device is shared, permissions need to be ored with existing */
			Permissions |= ((Security << XPPU_APERTURE_TRUSTZONE_OFFSET) | (Reqm->Params[0] & XPPU_APERTURE_PERMISSION_MASK));
		} else if (UsagePolicy == (u8)REQ_NO_RESTRICTION) {
			Permissions = (XPPU_APERTURE_PERMISSION_MASK | XPPU_APERTURE_TRUSTZONE_MASK);
		} else {
			/* Required due to MISRA */
			PmDbg("Invalid UsagePolicy %d\r\n", UsagePolicy);
		}
	} else {
		/* Configure XPPU to disable masters belonging to this subsystem */
		Permissions = (Permissions | XPPU_APERTURE_TRUSTZONE_MASK);
		Permissions = (Permissions & (~(Reqm->Params[0] & XPPU_APERTURE_PERMISSION_MASK)));
	}

	PmDbg("PermissionRegAddress %x Permissions %x RegMask %x \r\n",PermissionRegAddress, Permissions, PermissionRegMask);

	if ((PLATFORM_VERSION_SILICON == Platform) && (PLATFORM_VERSION_SILICON_ES1 == PlatformVersion)) {
		/* Set XPPU control to 0 */
		PmRmw32(PpuNode->ProtNode.Node.BaseAddress + XPPU_CTRL_OFFSET,
				XPPU_CTRL_ENABLE_MASK, ~XPPU_CTRL_ENABLE_MASK);

		/* Set Enable Permission check of the required apertures to 0 */
		PmRmw32(PermissionRegAddress, PermissionRegMask, 0);

		/* Program permissions of the apertures that need to be �reconfigured� */
		XPmProt_XppuSetAperture(PpuNode, ApertureAddress, Permissions);

		/* Enable back permission check of the apertures */
		PmRmw32(PermissionRegAddress, PermissionRegMask, PermissionRegMask);

		/* Set XPPU control to 1 */
		PmRmw32(PpuNode->ProtNode.Node.BaseAddress + XPPU_CTRL_OFFSET,
				XPPU_CTRL_ENABLE_MASK, XPPU_CTRL_ENABLE_MASK);

	} else {
		/* Configure Dynamic reconfig enable registers before changing XPPU config */
		PmOut32(PpuNode->ProtNode.Node.BaseAddress + XPPU_DYNAMIC_RECONFIG_APER_ADDR_OFFSET, DynamicReconfigAddrOffset);
		PmOut32(PpuNode->ProtNode.Node.BaseAddress + XPPU_DYNAMIC_RECONFIG_APER_PERM_OFFSET, Permissions);
		PmOut32(PpuNode->ProtNode.Node.BaseAddress + XPPU_DYNAMIC_RECONFIG_EN_OFFSET, 1);

		/* Write values to Aperture */
		XPmProt_XppuSetAperture(PpuNode, ApertureAddress, Permissions);

		/* Disable dynamic reconfig enable once done */
		PmOut32(PpuNode->ProtNode.Node.BaseAddress + XPPU_DYNAMIC_RECONFIG_EN_OFFSET, 0);
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Enable XMPU with given NodeId
 *
 * @param  NodeId: Node Id assigned to a XMPU node
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 ****************************************************************************/
static XStatus XPmProt_XmpuEnable(u32 NodeId)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddr;
	u32 RegVal;
	XPm_ProtMpu *MpuNode = NULL;

	MpuNode = (XPm_ProtMpu *)XPmProt_GetById(NodeId);
	if (NULL == MpuNode) {
		goto done;
	}

	if ((u8)XPM_PROT_ENABLED == MpuNode->ProtNode.Node.State) {
		Status = XST_SUCCESS;
		goto done;
	}

	/* XMPU base address */
	BaseAddr = MpuNode->ProtNode.Node.BaseAddress;

	/* Read region alignment */
	PmIn32(BaseAddr + XMPU_CTRL_OFFSET, RegVal);
	MpuNode->AlignCfg = (RegVal >> XMPU_CTRL_ALIGN_CFG_SHIFT) & 0x1U;

	/* Disable default RD/WR configuration on protected regions */
	PmRmw32(BaseAddr + XMPU_CTRL_OFFSET,
			XMPU_CTRL_DISABLE_DEFAULT_S_REGION_MASK,
			XMPU_CTRL_DISABLE_DEFAULT_S_REGION_MASK);

	PmIn32(BaseAddr + XMPU_CTRL_OFFSET, RegVal);

	PmDbg("XMPU NodeID: 0x%08x | CTRL: 0x%08x\r\n", NodeId, RegVal);

	/* Enable SW state */
	MpuNode->ProtNode.Node.State = (u8)XPM_PROT_ENABLED;

	Status = XST_SUCCESS;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Disable XMPU with given NodeId
 *
 * @param  NodeId: Node Id assigned to a XMPU node
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 ****************************************************************************/
static XStatus XPmProt_XmpuDisable(u32 NodeId)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddr, Region;
	u32 RegnCfgAddr = 0;
	XPm_ProtMpu *MpuNode = NULL;

	MpuNode = (XPm_ProtMpu *)XPmProt_GetById(NodeId);
	if (NULL == MpuNode) {
		goto done;
	}

	/* XMPU base address */
	BaseAddr = MpuNode->ProtNode.Node.BaseAddress;

	/* Starting region config address */
	RegnCfgAddr = BaseAddr + XMPU_R00_CONFIG_OFFSET;

	/* Disable all the regions */
	for (Region = 0; Region < MAX_MEM_REGIONS; Region++) {
		/* Disable all memory regions */
		RegnCfgAddr += REGN_CONFIG_OFFSET(Region);
		PmRmw32(RegnCfgAddr,
			XMPU_RXX_CONFIG_ENABLE_MASK,
			~XMPU_RXX_CONFIG_ENABLE_MASK);
	}

	/* Disable SW state */
	MpuNode->ProtNode.Node.State = (u8)XPM_PROT_DISABLED;

	Status = XST_SUCCESS;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Setup a XMPU region with given subsystem requirements and enable it
 *
 * @param  Reqm: device requirement imposed by caller subsystem
 * @param  RegionId: Region Id (0-15)
 * @param  Enable: enable(1)/disable(0) region
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 * @note   An assumption: @Reqm and @MpuNode is Non-NULL
 *
 ****************************************************************************/
static XStatus XPmProt_XmpuSetupRegion(const XPm_Requirement *Reqm,
				const XPm_ProtMpu *MpuNode,
				u32 RegionId,
				u32 Enable)
{
	u32 Status = XST_FAILURE;
	u32 CfgToWr, RegnCfgAddr;
	u8 Usage, Security, RdAllowed, WrAllowed, NSRegnCheck;

	if (MAX_MEM_REGIONS <= RegionId) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	RegnCfgAddr = MpuNode->ProtNode.Node.BaseAddress
		+ XMPU_R00_CONFIG_OFFSET
		+ REGN_CONFIG_OFFSET(RegionId);

	/* Requirement flags */
	Usage = USAGE_POLICY(Reqm->Flags);
	Security = SECURITY_POLICY(Reqm->Flags);
	RdAllowed = RD_POLICY(Reqm->Flags);
	WrAllowed = WR_POLICY(Reqm->Flags);
	NSRegnCheck = REGN_CHECK_POLICY(Reqm->Flags);

	PmDbg("Configuring region: %u\r\n", RegionId);
	PmDbg("Usage: 0x%x, Security: 0x%x, Rd: 0x%x, Wr: 0x%x, Check: 0x%x\r\n",
			Usage, Security, RdAllowed, WrAllowed, NSRegnCheck);

	/* Setup config to be written; except enable */
	CfgToWr = (((NSRegnCheck << XMPU_RXX_CONFIG_NSCHECKTYPE_SHIFT)
			& XMPU_RXX_CONFIG_NSCHECKTYPE_MASK)
		 | ((Security << XMPU_RXX_CONFIG_REGIONNS_SHIFT)
			 & XMPU_RXX_CONFIG_REGIONNS_MASK)
		 | ((WrAllowed << XMPU_RXX_CONFIG_WRALLOWED_SHIFT)
			 & XMPU_RXX_CONFIG_WRALLOWED_MASK)
		 | ((RdAllowed << XMPU_RXX_CONFIG_RDALLOWED_SHIFT)
			 & XMPU_RXX_CONFIG_RDALLOWED_MASK));

	/* Setup memory region config */
	if ((u8)REQ_NONSHARED != Usage) {
		/**
		 * TODO: Add support for following usage modes:
		 *   - REQ_SHARED
		 *   - REQ_TIME_SHARED
		 *   - REQ_NO_RESTRICTION
		 */
		Status = XST_NO_FEATURE;
		goto done;
	} else {
		/**
		 * Only REQ_NONSHARED usage mode is supported for now;
		 * so no reconfiguration of Masters is required.
		 * Therefore, allow all the configured masters belonging
		 * to this subsystem to access this region.
		 */
		CfgToWr |= (Enable & XMPU_RXX_CONFIG_ENABLE_MASK);
		PmOut32(RegnCfgAddr, CfgToWr);
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Configure XMPU according to access control policies from
 *         subsystem requirement for given memory region
 *
 * @param  Reqm: memory region device requirement
 *               imposed by caller subsystem
 * @param  Enable: enable(1)/disable(0) region
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 ****************************************************************************/
static XStatus XPmProt_XmpuConfigure(XPm_Requirement *Reqm, u32 Enable)
{
	XStatus Status = XST_FAILURE;
	u64 RegnStart, RegnEnd, DevStart, DevEnd;
	u32 BaseAddr, DeviceId, Region;
	u32 RegnStartLo, RegnStartHi, RegnStartLoVal, RegnStartHiVal;
	u32 RegnEndLo, RegnEndHi, RegnEndLoVal, RegnEndHiVal;
	XPm_ProtMpu *MpuNode = NULL;
	XPm_MemDevice *MemDevice = NULL;

	u8 XmpuIdx;

	if ((NULL == Reqm) || (NULL == Reqm->Device)) {
		Status = XPM_ERR_DEVICE;
		goto done;
	}
	MemDevice = (XPm_MemDevice *)Reqm->Device;
	DevStart  = (u64)(MemDevice->StartAddress);
	DevEnd    = (u64)(MemDevice->EndAddress);

	PmDbg("StartAddress: 0x%08x - EndAddress: 0x%08x\r\n",
			MemDevice->StartAddress,
			MemDevice->EndAddress);
	PmDbg("DevStart: 0x%08x - DevEnd: 0x%08x\r\n",
			(u32)DevStart, (u32)DevEnd);

	/* Get relevant XMPU index */
	DeviceId = Reqm->Device->Node.Id;
	XmpuIdx = MEM_TYPE_TO_XMPU(DeviceId);

	PmDbg("DeviceId: 0x%08x | XMPU IDX: %d\r\n", DeviceId, XmpuIdx);

	MpuNode = (XPm_ProtMpu *)XPmProt_GetByIndex(XmpuIdx);
	if (NULL == MpuNode) {
		goto done;
	}

	/* Return if XMPU is not enabled */
	if ((u8)XPM_PROT_DISABLED == MpuNode->ProtNode.Node.State) {
		Status = XST_SUCCESS;
		goto done;
	}
	BaseAddr = MpuNode->ProtNode.Node.BaseAddress;

	/* Region-0 addresses */
	RegnStartLo = BaseAddr + XMPU_R00_START_LO_OFFSET;
	RegnStartHi = BaseAddr + XMPU_R00_START_HI_OFFSET;
	RegnEndLo   = BaseAddr + XMPU_R00_END_LO_OFFSET;
	RegnEndHi   = BaseAddr + XMPU_R00_END_HI_OFFSET;

	/**
	 * Iterate over XMPU regions
	 */
	for (Region = 0; Region < MAX_MEM_REGIONS; Region++) {
		/* compute region addresses */
		RegnStartLo += REGN_ADDR_OFFSET(Region);
		RegnStartHi += REGN_ADDR_OFFSET(Region);
		RegnEndLo   += REGN_ADDR_OFFSET(Region);
		RegnEndHi   += REGN_ADDR_OFFSET(Region);

		/* retrieve region start and end addresses */
		PmIn32(RegnStartLo, RegnStartLoVal);
		PmIn32(RegnStartHi, RegnStartHiVal);
		PmIn32(RegnEndLo, RegnEndLoVal);
		PmIn32(RegnEndHi, RegnEndHiVal);

		RegnStart = (((u64)RegnStartHiVal) << 32U) | RegnStartLoVal;
		RegnEnd   = (((u64)RegnEndHiVal) << 32U) | RegnEndLoVal;

		/**
		 * Enable/Disable all the regions which are configured.
		 */
		if ((DevStart < DevEnd)
		&&  (DevStart >= RegnStart)
		&&  (DevEnd <= RegnEnd)) {
			/* Configure and enable/disable the region based on requirements */
			Status = XPmProt_XmpuSetupRegion(Reqm, MpuNode, Region, Enable);
			if (XST_SUCCESS != Status) {
				goto done;
			}
		}
	}

	/**
	 * Memory region is not found in configured XMPU regions,
	 * however just return with success since HW will handle the case.
	 */
	Status = XST_SUCCESS;

done:
	if (XST_SUCCESS != Status) {
		PmErr("Returned: %x\r\n", Status);
	}

	return Status;
}

/****************************************************************************/
/**
 * @brief  Configure XMPU/XPPU according to access control policies from
 *         given subsystem requirement for memory regions/peripherals
 *
 * @param  Reqm: peripheral/memory region device requirement
 *               imposed by caller subsystem
 * @param  Enable: enable(1)/disable(0)
 *            - Masters in case of XPPU
 *            - Region in case of XMPU
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 * @note  This function is called from device request-release infrastructure;
 *        with the requirement specified from the subsystem requesting
 *        the device.
 *
 ****************************************************************************/
XStatus XPmProt_Configure(XPm_Requirement *Reqm, u32 Enable)
{
	XStatus Status = XST_FAILURE;
	u32 DeviceId;

	if ((NULL == Reqm) || (NULL == Reqm->Device)) {
		goto done;
	}
	DeviceId = Reqm->Device->Node.Id;

	if (PLATFORM_VERSION_SILICON != Platform) {
		Status = XST_SUCCESS;
		goto done;
	}

	/* Configure XPPU for peripheral devices */
	if ((u32)XPM_NODESUBCL_DEV_PERIPH == NODESUBCLASS(DeviceId)) {
		Status = XPmProt_XppuConfigure(Reqm, Enable);
	/* Configure XMPU for memory regions */
	} else if (1U == IS_MEM_REGN(DeviceId)) {
		/**
		 * TODO:
		 * Add support for protecting FPD Slave peripherals protected by XMPU.
		 * Current support is only for OCM and DDR memory regions.
		 */
		Status = XPmProt_XmpuConfigure(Reqm, Enable);
	} else {
		Status = XST_SUCCESS;
		goto done;
	}

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Common handler for XPPU enable/disable control
 *
 * @param  Args: Args list
 *		Arg0 - Xppu NodeId
 *		Arg1 - Enable(1)/Disable(0) bit
 *		Arg2 - Default permission mask for all apertures
 * @param  NumOfArgs - total number of args present in Args list
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 * @note  This function is common handler for enabling or disabling
 * an instance of XPPU. This will be called as a part of respective
 * power domain's INIT NODE handling routine sequences.
 *
 ****************************************************************************/
XStatus XPmProt_CommonXppuCtrl(u32 *Args, u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 XppuNodeId, Enable;

	if ((NULL == Args) || (NumOfArgs < 2U)) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	XppuNodeId = Args[0];
	Enable = Args[1];

	if (((u32)XPM_NODECLASS_PROTECTION != NODECLASS(XppuNodeId))
	||  ((u32)XPM_NODESUBCL_PROT_XPPU != NODESUBCLASS(XppuNodeId))) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	if ((1U == Enable) && (3U == NumOfArgs)) {
		Status = XPmProt_XppuEnable(XppuNodeId, Args[2]);
	} else {
		Status = XPmProt_XppuDisable(XppuNodeId);
	}

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief  Common handler for XMPU enable/disable control
 *
 * @param  Args: Args list
 *		Arg0 - Xmpu NodeId
 *		Arg1 - Enable(1)/Disable(0) bit
 * @param  NumOfArgs - total number of args present in Args list
 *
 * @return XST_SUCCESS if successful else appropriate failure code
 *
 * @note  This function is common handler for enabling or disabling
 * an instance of XMPU. This will be called as a part of respective
 * power domain's INIT NODE handling routine sequences.
 *
 ****************************************************************************/
XStatus XPmProt_CommonXmpuCtrl(u32 *Args, u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 XmpuNodeId, Enable;

	if ((NULL == Args) || (2U != NumOfArgs)) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	XmpuNodeId = Args[0];
	Enable = Args[1];

	if (((u32)XPM_NODECLASS_PROTECTION != NODECLASS(XmpuNodeId))
	||  ((u32)XPM_NODESUBCL_PROT_XMPU != NODESUBCLASS(XmpuNodeId))) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	if (1U == Enable) {
		Status = XPmProt_XmpuEnable(XmpuNodeId);
	} else {
		Status = XPmProt_XmpuDisable(XmpuNodeId);
	}

done:
	return Status;
}
