#include "common/common.h"
#include "common/json.h"
#include "archo.h"
#include "signing.h"
#include <sstream>

size_t execSegLimit = 0;

static inline void put(std::streambuf &stream, uint8_t value) {
    assert(stream.sputc(value) != EOF);
}

static inline void put(std::streambuf &stream, const void *data, size_t size) {
    assert(stream.sputn(static_cast<const char *>(data), size) == size);
}

static inline void put(std::streambuf &stream, const std::string &data) {
    return put(stream, data.data(), data.size());
}

static inline unsigned bytes(uint64_t value) {
    return (64 - __builtin_clzll(value) + 7) / 8;
}

static void put(std::streambuf &stream, uint64_t value, size_t length) {
    length *= 8;
    do put(stream, uint8_t(value >> (length -= 8)));
    while (length != 0);
}

static void der(std::streambuf &stream, uint64_t value) {
    if (value < 128)
        put(stream, value);
    else {
        unsigned length(bytes(value));
        put(stream, 0x80 | length);
        put(stream, value, length);
    }
}

static std::string der(uint8_t tag, const char *value, size_t length) {
    std::stringbuf data;
    put(data, tag);
    der(data, length);
    put(data, value, length);
    return data.str();
}

static std::string der(uint8_t tag, const char *value) {
    return der(tag, value, strlen(value)); }
static std::string der(uint8_t tag, const std::string &value) {
    return der(tag, value.data(), value.size()); }

template <typename Type_>
static void der_(std::stringbuf &data, const Type_ &values) {
    size_t size(0);
    for (const auto &value : values)
        size += value.size();
    der(data, size);
    for (const auto &value : values)
        put(data, value);
}

static std::string der(const std::vector<std::string> &values) {
    std::stringbuf data;
    put(data, 0x30);
    der_(data, values);
    return data.str();
}

static std::string der(const std::multiset<std::string> &values) {
    std::stringbuf data;
    put(data, 0x31);
    der_(data, values);
    return data.str();
}

static std::string der(const std::pair<std::string, std::string> &value) {
    const auto key(der(0x0c, value.first));
    std::stringbuf data;
    put(data, 0x30);
    der(data, key.size() + value.second.size());
    put(data, key);
    put(data, value.second);
    return data.str();
}

static std::string der(JValue data) {
    switch (const auto type = data.type()) {
        case JValue::E_BOOL: {
            bool value = data.asBool();

            std::stringbuf data;
            put(data, 0x01);
            der(data, 1);
            put(data, value ? 1 : 0);
            return data.str();
        } break;

        case JValue::E_INT: {
            uint64_t value;
			value = data.asInt64();
            const auto length(bytes(value));

            std::stringbuf data;
            put(data, 0x02);
            der(data, length);
            put(data, value, length);
            return data.str();
        } break;

        case JValue::E_FLOAT: {
            assert(false);
        } break;

        case JValue::E_DATE: {
            assert(false);
        } break;

        case JValue::E_DATA: {
			assert(false);
        } break;

        case JValue::E_STRING: {
            const char *value;
			value = data.asCString();
            return der(0x0c, value);
        } break;

        case JValue::E_ARRAY: {
            std::vector<std::string> values;
			for (size_t i = 0; i < data.size(); i++)
			{
				values.push_back(der(data[i]));
			}

            return der(values);
        } break;

        case JValue::E_OBJECT: {
            std::multiset<std::string> values;

			std::vector<string> keys;
			data.keys(keys);
			for (auto iter = keys.begin(); iter != keys.end(); iter++)
			{
				string key = *iter;
				values.insert(der(std::make_pair(key, der(data[key]))));
			}

			return der(values);
        } break;

        default: {
            assert(false && "unsupported plist type");
        } break;
    }
}

static std::string der_blob(JValue data) {
	string strEntitlements = der(data);

	uint32_t magic = MAGIC_EMBEDDED_ENTITLEMENTS_DER;
    uint32_t length; // Total length, _including_ .magic and .length
	length = sizeof(uint32_t) * 2 + strEntitlements.size();

	std::stringbuf blobData;
	put(blobData, magic, sizeof(uint32_t));
	put(blobData, length, sizeof(uint32_t));
	put(blobData, strEntitlements);

	return blobData.str();
}

ZArchO::ZArchO()
{
	m_pBase = NULL;
	m_uLength = 0;
	m_uCodeLength = 0;
	m_pSignBase = NULL;
	m_uSignLength = 0;
	m_pHeader = NULL;
	m_uHeaderSize = 0;
	m_bEncrypted = false;
	m_b64 = false;
	m_bBigEndian = false;
	m_bEnoughSpace = true;
	m_pCodeSignSegment = NULL;
	m_pLinkEditSegment = NULL;
	m_uLoadCommandsFreeSpace = 0;
}

bool ZArchO::Init(uint8_t *pBase, uint32_t uLength)
{
	if (NULL == pBase || uLength <= 0)
	{
		return false;
	}

	m_pBase = pBase;
	m_uLength = uLength;
	m_uCodeLength = (uLength % 16 == 0) ? uLength : uLength + 16 - (uLength % 16);
	m_pHeader = (mach_header *)m_pBase;
	m_b64 = (MH_MAGIC_64 == m_pHeader->magic || MH_CIGAM_64 == m_pHeader->magic) ? true : false;
	m_bBigEndian = (MH_CIGAM == m_pHeader->magic || MH_CIGAM_64 == m_pHeader->magic) ? true : false;
	m_uHeaderSize = m_b64 ? sizeof(mach_header_64) : sizeof(mach_header);

	uint8_t *pLoadCommand = m_pBase + m_uHeaderSize;
	for (uint32_t i = 0; i < BO(m_pHeader->ncmds); i++)
	{
		load_command *plc = (load_command *)pLoadCommand;
		switch (BO(plc->cmd))
		{
		case LC_SEGMENT:
		{
			segment_command *seglc = (segment_command *)pLoadCommand;
			if (0 == strcmp("__TEXT", seglc->segname))
			{
				execSegLimit = seglc->vmsize;
				for (uint32_t j = 0; j < BO(seglc->nsects); j++)
				{
					section *sect = (section *)((pLoadCommand + sizeof(segment_command)) + sizeof(section) * j);
					if (0 == strcmp("__text", sect->sectname))
					{
						if (BO(sect->offset) > (BO(m_pHeader->sizeofcmds) + m_uHeaderSize))
						{
							m_uLoadCommandsFreeSpace = BO(sect->offset) - BO(m_pHeader->sizeofcmds) - m_uHeaderSize;
						}
					}
					else if (0 == strcmp("__info_plist", sect->sectname))
					{
						m_strInfoPlist.append((const char *)m_pBase + BO(sect->offset), BO(sect->size));
					}
				}
			}
			else if (0 == strcmp("__LINKEDIT", seglc->segname))
			{
				m_pLinkEditSegment = pLoadCommand;
			}
		}
		break;
		case LC_SEGMENT_64:
		{
			segment_command_64 *seglc = (segment_command_64 *)pLoadCommand;
			if (0 == strcmp("__TEXT", seglc->segname))
			{
				execSegLimit = seglc->vmsize;
				for (uint32_t j = 0; j < BO(seglc->nsects); j++)
				{
					section_64 *sect = (section_64 *)((pLoadCommand + sizeof(segment_command_64)) + sizeof(section_64) * j);
					if (0 == strcmp("__text", sect->sectname))
					{
						if (BO(sect->offset) > (BO(m_pHeader->sizeofcmds) + m_uHeaderSize))
						{
							m_uLoadCommandsFreeSpace = BO(sect->offset) - BO(m_pHeader->sizeofcmds) - m_uHeaderSize;
						}
					}
					else if (0 == strcmp("__info_plist", sect->sectname))
					{
						m_strInfoPlist.append((const char *)m_pBase + BO(sect->offset), BO(sect->size));
					}
				}
			}
			else if (0 == strcmp("__LINKEDIT", seglc->segname))
			{
				m_pLinkEditSegment = pLoadCommand;
			}
		}
		break;
		case LC_ENCRYPTION_INFO:
		case LC_ENCRYPTION_INFO_64:
		{
			encryption_info_command *crypt_cmd = (encryption_info_command *)pLoadCommand;
			if (BO(crypt_cmd->cryptid) >= 1)
			{
				m_bEncrypted = true;
			}
		}
		break;
		case LC_CODE_SIGNATURE:
		{
			codesignature_command *pcslc = (codesignature_command *)pLoadCommand;
			m_pCodeSignSegment = pLoadCommand;
			m_uCodeLength = BO(pcslc->dataoff);
			m_pSignBase = m_pBase + m_uCodeLength;
			m_uSignLength = GetCodeSignatureLength(m_pSignBase);
		}
		break;
		}

		pLoadCommand += BO(plc->cmdsize);
	}

	return true;
}

const char *ZArchO::GetArch(int cpuType, int cpuSubType)
{
	switch (cpuType)
	{
	case CPU_TYPE_ARM:
	{
		switch (cpuSubType)
		{
		case CPU_SUBTYPE_ARM_V6:
			return "armv6";
			break;
		case CPU_SUBTYPE_ARM_V7:
			return "armv7";
			break;
		case CPU_SUBTYPE_ARM_V7S:
			return "armv7s";
			break;
		case CPU_SUBTYPE_ARM_V7K:
			return "armv7k";
			break;
		case CPU_SUBTYPE_ARM_V8:
			return "armv8";
			break;
		}
	}
	break;
	case CPU_TYPE_ARM64:
	{
		switch (cpuSubType)
		{
		case CPU_SUBTYPE_ARM64_ALL:
			return "arm64";
			break;
		case CPU_SUBTYPE_ARM64_V8:
			return "arm64v8";
			break;
		case 2:
			return "arm64e";
			break;
		}
	}
	break;
	case CPU_TYPE_ARM64_32:
	{
		switch (cpuSubType)
		{
		case CPU_SUBTYPE_ARM64_ALL:
			return "arm64_32";
			break;
		case CPU_SUBTYPE_ARM64_32_V8:
			return "arm64e_32";
			break;
		}
	}
	break;
	case CPU_TYPE_X86:
	{
		switch (cpuSubType)
		{
		default:
			return "x86_32";
			break;
		}
	}
	break;
	case CPU_TYPE_X86_64:
	{
		switch (cpuSubType)
		{
		default:
			return "x86_64";
			break;
		}
	}
	break;
	}
	return "unknown";
}

const char *ZArchO::GetFileType(uint32_t uFileType)
{
	switch (uFileType)
	{
	case MH_OBJECT:
		return "MH_OBJECT";
		break;
	case MH_EXECUTE:
		return "MH_EXECUTE";
		break;
	case MH_FVMLIB:
		return "MH_FVMLIB";
		break;
	case MH_CORE:
		return "MH_CORE";
		break;
	case MH_PRELOAD:
		return "MH_PRELOAD";
		break;
	case MH_DYLIB:
		return "MH_DYLIB";
		break;
	case MH_DYLINKER:
		return "MH_DYLINKER";
		break;
	case MH_BUNDLE:
		return "MH_BUNDLE";
		break;
	case MH_DYLIB_STUB:
		return "MH_DYLIB_STUB";
		break;
	case MH_DSYM:
		return "MH_DSYM";
		break;
	case MH_KEXT_BUNDLE:
		return "MH_KEXT_BUNDLE";
		break;
	}
	return "MH_UNKNOWN";
}

uint32_t ZArchO::BO(uint32_t uValue)
{
	return m_bBigEndian ? LE(uValue) : uValue;
}

bool ZArchO::IsExecute()
{
	if (NULL != m_pHeader)
	{
		return (MH_EXECUTE == BO(m_pHeader->filetype));
	}
	return false;
}

void ZArchO::PrintInfo()
{
	if (NULL == m_pHeader)
	{
		return;
	}

	ZLog::Print("------------------------------------------------------------------\n");
	ZLog::Print(">>> MachO Info: \n");
	ZLog::PrintV("\tFileType: \t%s\n", GetFileType(BO(m_pHeader->filetype)));
	ZLog::PrintV("\tTotalSize: \t%u (%s)\n", m_uLength, FormatSize(m_uLength).c_str());
	ZLog::PrintV("\tPlatform: \t%u\n", m_b64 ? 64 : 32);
	ZLog::PrintV("\tCPUArch: \t%s\n", GetArch(BO(m_pHeader->cputype), BO(m_pHeader->cpusubtype)));
	ZLog::PrintV("\tCPUType: \t0x%x\n", BO(m_pHeader->cputype));
	ZLog::PrintV("\tCPUSubType: \t0x%x\n", BO(m_pHeader->cpusubtype));
	ZLog::PrintV("\tBigEndian: \t%d\n", m_bBigEndian);
	ZLog::PrintV("\tEncrypted: \t%d\n", m_bEncrypted);
	ZLog::PrintV("\tCommandCount: \t%d\n", BO(m_pHeader->ncmds));
	ZLog::PrintV("\tCodeLength: \t%d (%s)\n", m_uCodeLength, FormatSize(m_uCodeLength).c_str());
	ZLog::PrintV("\tSignLength: \t%d (%s)\n", m_uSignLength, FormatSize(m_uSignLength).c_str());
	ZLog::PrintV("\tSpareLength: \t%d (%s)\n", m_uLength - m_uCodeLength - m_uSignLength, FormatSize(m_uLength - m_uCodeLength - m_uSignLength).c_str());
	
	uint8_t *pLoadCommand = m_pBase + m_uHeaderSize;
	for (uint32_t i = 0; i < BO(m_pHeader->ncmds); i++)
	{
		load_command *plc = (load_command *)pLoadCommand;
		if (LC_VERSION_MIN_IPHONEOS == BO(plc->cmd))
		{
			ZLog::PrintV("\tMIN_IPHONEOS: \t0x%x\n", *((uint32_t*)(pLoadCommand + sizeof(load_command))));
		}
		else if(LC_RPATH == BO(plc->cmd))
		{
			ZLog::PrintV("\tLC_RPATH: \t%s\n", (char*)(pLoadCommand + sizeof(load_command) + 4));
		}
		pLoadCommand += BO(plc->cmdsize);
	}

	bool bHasWeakDylib = false;
	ZLog::PrintV("\tLC_LOAD_DYLIB: \n");
	pLoadCommand = m_pBase + m_uHeaderSize;
	for (uint32_t i = 0; i < BO(m_pHeader->ncmds); i++)
	{
		load_command *plc = (load_command *)pLoadCommand;
		if (LC_LOAD_DYLIB == BO(plc->cmd))
		{
			dylib_command *dlc = (dylib_command *)pLoadCommand;
			const char *szDyLib = (const char *)(pLoadCommand + BO(dlc->dylib.name.offset));
			ZLog::PrintV("\t\t\t%s\n", szDyLib);
		}
		else if (LC_LOAD_WEAK_DYLIB == BO(plc->cmd))
		{
			bHasWeakDylib = true;
		}
		pLoadCommand += BO(plc->cmdsize);
	}

	if(bHasWeakDylib)
	{
		ZLog::PrintV("\tLC_LOAD_WEAK_DYLIB: \n");
		pLoadCommand = m_pBase + m_uHeaderSize;
		for (uint32_t i = 0; i < BO(m_pHeader->ncmds); i++)
		{
			load_command *plc = (load_command *)pLoadCommand;
			if (LC_LOAD_WEAK_DYLIB == BO(plc->cmd))
			{
				dylib_command *dlc = (dylib_command *)pLoadCommand;
				const char *szDyLib = (const char *)(pLoadCommand + BO(dlc->dylib.name.offset));
				ZLog::PrintV("\t\t\t%s (weak)\n", szDyLib);
			}
			pLoadCommand += BO(plc->cmdsize);
		}
	}

	if (!m_strInfoPlist.empty())
	{
		ZLog::Print("\n>>> Embedded Info.plist: \n");
		ZLog::PrintV("\tlength: \t%lu\n", m_strInfoPlist.size());

		string strInfoPlist = m_strInfoPlist;
		PWriter::StringReplace(strInfoPlist, "\n", "\n\t\t\t");
		ZLog::PrintV("\tcontent: \t%s\n", strInfoPlist.c_str());

		PrintDataSHASum("\tSHA-1:  \t", E_SHASUM_TYPE_1, m_strInfoPlist);
		PrintDataSHASum("\tSHA-256:\t", E_SHASUM_TYPE_256, m_strInfoPlist);
	}

	if (NULL == m_pSignBase || m_uSignLength <= 0)
	{
		ZLog::Warn(">>> Can't Find CodeSignature Segment!\n");
	}
	else
	{
		ParseCodeSignature(m_pSignBase);
	}

	ZLog::Print("------------------------------------------------------------------\n");
}

bool ZArchO::BuildCodeSignature(ZSignAsset *pSignAsset, bool bForce, const string &strBundleId, const string &strInfoPlistSHA1, const string &strInfoPlistSHA256, const string &strCodeResourcesSHA1, const string &strCodeResourcesSHA256, string &strOutput)
{
	string strRequirementsSlot;
	string strEntitlementsSlot;
	string strEntitlementsDerFormatSlot;

	string emptyEntitlementStr = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict/>\n</plist>\n";
	SlotBuildRequirements(strBundleId, pSignAsset->m_strSubjectCN, strRequirementsSlot);
	SlotBuildEntitlements(IsExecute() ? pSignAsset->m_strEntitlementsData : emptyEntitlementStr, strEntitlementsSlot);

	if (IsExecute() && !pSignAsset->m_strEntitlementsData.empty()) {
        //parse entitlementDataStr to entitlementBlobs
        JValue jvInfo;
        jvInfo.readPList(pSignAsset->m_strEntitlementsData);
		string strDerBlob = der_blob(jvInfo);
        
        //
        strEntitlementsDerFormatSlot.clear();
        strEntitlementsDerFormatSlot.append(strDerBlob.data(), strDerBlob.size());
    }

	string strRequirementsSlotSHA1;
	string strRequirementsSlotSHA256;
	if(strRequirementsSlot.empty())
	{//empty
		strRequirementsSlotSHA1.append(20, 0);
		strRequirementsSlotSHA256.append(32, 0);
	}
	else
	{
		SHASum(strRequirementsSlot, strRequirementsSlotSHA1, strRequirementsSlotSHA256);
	}
	
	string strEntitlementsSlotSHA1;
	string strEntitlementsSlotSHA256;
	if(strEntitlementsSlot.empty())
	{//empty
		strEntitlementsSlotSHA1.append(20, 0);
		strEntitlementsSlotSHA256.append(32, 0);
	}
	else
	{
		SHASum(strEntitlementsSlot, strEntitlementsSlotSHA1, strEntitlementsSlotSHA256);
	}

	string strEntitlementsDerSHA1;
    string strEntitlementsDerSHA256;
    if (strEntitlementsDerFormatSlot.empty()) {//empty
        strEntitlementsDerSHA1.append(20, 0);
        strEntitlementsDerSHA256.append(32, 0);
    } else {
        SHASum(strEntitlementsDerFormatSlot, strEntitlementsDerSHA1, strEntitlementsDerSHA256);
    }

	uint8_t *pCodeSlots1Data = NULL;
	uint8_t *pCodeSlots256Data = NULL;
	uint32_t uCodeSlots1DataLength = 0;
	uint32_t uCodeSlots256DataLength = 0;
	if (!bForce)
	{
		GetCodeSignatureExistsCodeSlotsData(m_pSignBase, pCodeSlots1Data, uCodeSlots1DataLength, pCodeSlots256Data, uCodeSlots256DataLength);
	}

	uint64_t execSegFlags = 0;
	if (strstr(strEntitlementsSlot.data() + 8, "<key>get-task-allow</key>") != NULL) {
		// TODO: Check if get-task-allow is actually set to true
		execSegFlags = CS_EXECSEG_MAIN_BINARY | CS_EXECSEG_ALLOW_UNSIGNED;
	}

	string strCMSSignatureSlot;
	string strCodeDirectorySlot;
	string strAltnateCodeDirectorySlot;
	SlotBuildCodeDirectory(false, m_pBase, m_uCodeLength, pCodeSlots1Data, uCodeSlots1DataLength, execSegLimit, execSegFlags, strBundleId, pSignAsset->m_strTeamId, strInfoPlistSHA1, strRequirementsSlotSHA1, strCodeResourcesSHA1, strEntitlementsSlotSHA1, strEntitlementsDerSHA1, IsExecute(), strCodeDirectorySlot);
	SlotBuildCodeDirectory(true, m_pBase, m_uCodeLength, pCodeSlots256Data, uCodeSlots256DataLength, execSegLimit, execSegFlags, strBundleId, pSignAsset->m_strTeamId, strInfoPlistSHA256, strRequirementsSlotSHA256, strCodeResourcesSHA256, strEntitlementsSlotSHA256, strEntitlementsDerSHA256, IsExecute(), strAltnateCodeDirectorySlot);
	SlotBuildCMSSignature(pSignAsset, strCodeDirectorySlot, strAltnateCodeDirectorySlot, strCMSSignatureSlot);

	uint32_t uCodeDirectorySlotLength = (uint32_t)strCodeDirectorySlot.size();
	uint32_t uRequirementsSlotLength = (uint32_t)strRequirementsSlot.size();
	uint32_t uEntitlementsSlotLength = (uint32_t)strEntitlementsSlot.size();
	uint32_t uEntitlementsDerLength = (uint32_t)strEntitlementsDerFormatSlot.size();
	uint32_t uAltnateCodeDirectorySlotLength = (uint32_t)strAltnateCodeDirectorySlot.size();
	uint32_t uCMSSignatureSlotLength = (uint32_t)strCMSSignatureSlot.size();

	uint32_t uCodeSignBlobCount = 0;
	uCodeSignBlobCount += (uCodeDirectorySlotLength > 0) ? 1 : 0;
	uCodeSignBlobCount += (uRequirementsSlotLength > 0) ? 1 : 0;
	uCodeSignBlobCount += (uEntitlementsSlotLength > 0) ? 1 : 0;
	uCodeSignBlobCount += (uEntitlementsDerLength > 0) ? 1 : 0;
	uCodeSignBlobCount += (uAltnateCodeDirectorySlotLength > 0) ? 1 : 0;
	uCodeSignBlobCount += (uCMSSignatureSlotLength > 0) ? 1 : 0;

	uint32_t uSuperBlobHeaderLength = sizeof(CS_SuperBlob) + uCodeSignBlobCount * sizeof(CS_BlobIndex);
	uint32_t uCodeSignLength = uSuperBlobHeaderLength + uCodeDirectorySlotLength + uRequirementsSlotLength + uEntitlementsSlotLength + uEntitlementsDerLength + uAltnateCodeDirectorySlotLength + uCMSSignatureSlotLength;

	vector<CS_BlobIndex> arrBlobIndexes;
	if (uCodeDirectorySlotLength > 0)
	{
		CS_BlobIndex blob;
		blob.type = BE(CSSLOT_CODEDIRECTORY);
		blob.offset = BE(uSuperBlobHeaderLength);
		arrBlobIndexes.push_back(blob);
	}
	if (uRequirementsSlotLength > 0)
	{
		CS_BlobIndex blob;
		blob.type = BE(CSSLOT_REQUIREMENTS);
		blob.offset = BE(uSuperBlobHeaderLength + uCodeDirectorySlotLength);
		arrBlobIndexes.push_back(blob);
	}
	if (uEntitlementsSlotLength > 0)
	{
		CS_BlobIndex blob;
		blob.type = BE(CSSLOT_ENTITLEMENTS);
		blob.offset = BE(uSuperBlobHeaderLength + uCodeDirectorySlotLength + uRequirementsSlotLength);
		arrBlobIndexes.push_back(blob);
	}
	if (uEntitlementsDerLength > 0) {
        CS_BlobIndex blob;
        blob.type = BE(CSSLOT_ENTITLEMENTS_DER);
        blob.offset = BE(uSuperBlobHeaderLength + uCodeDirectorySlotLength + uRequirementsSlotLength+ uEntitlementsSlotLength);
        arrBlobIndexes.push_back(blob);
    }
	if (uAltnateCodeDirectorySlotLength > 0)
	{
		CS_BlobIndex blob;
		blob.type = BE(CSSLOT_ALTERNATE_CODEDIRECTORIES);
		blob.offset = BE(uSuperBlobHeaderLength + uCodeDirectorySlotLength + uRequirementsSlotLength + uEntitlementsSlotLength + uEntitlementsDerLength);
		arrBlobIndexes.push_back(blob);
	}
	if (uCMSSignatureSlotLength > 0)
	{
		CS_BlobIndex blob;
		blob.type = BE(CSSLOT_SIGNATURESLOT);
		blob.offset = BE(uSuperBlobHeaderLength + uCodeDirectorySlotLength + uRequirementsSlotLength + uEntitlementsSlotLength + uEntitlementsDerLength + uAltnateCodeDirectorySlotLength);
		arrBlobIndexes.push_back(blob);
	}

	CS_SuperBlob superblob;
	superblob.magic = BE(CSMAGIC_EMBEDDED_SIGNATURE);
	superblob.length = BE(uCodeSignLength);
	superblob.count = BE(uCodeSignBlobCount);

	strOutput.clear();
	strOutput.reserve(uCodeSignLength);
	strOutput.append((const char *)&superblob, sizeof(superblob));
	for (size_t i = 0; i < arrBlobIndexes.size(); i++)
	{
		CS_BlobIndex &blob = arrBlobIndexes[i];
		strOutput.append((const char *)&blob, sizeof(blob));
	}
	strOutput += strCodeDirectorySlot;
	strOutput += strRequirementsSlot;
	strOutput += strEntitlementsSlot;
	strOutput += strEntitlementsDerFormatSlot;
	strOutput += strAltnateCodeDirectorySlot;
	strOutput += strCMSSignatureSlot;

	if (ZLog::IsDebug())
	{
		WriteFile("./.zsign_debug/Requirements.slot.new", strRequirementsSlot);
		WriteFile("./.zsign_debug/Entitlements.slot.new", strEntitlementsSlot);
		WriteFile("./.zsign_debug/Entitlements.plist.new", strEntitlementsSlot.data() + 8, strEntitlementsSlot.size() - 8);
		WriteFile("./.zsign_debug/CodeDirectory_SHA1.slot.new", strCodeDirectorySlot);
		WriteFile("./.zsign_debug/CodeDirectory_SHA256.slot.new", strAltnateCodeDirectorySlot);
		WriteFile("./.zsign_debug/CMSSignature.slot.new", strCMSSignatureSlot);
		WriteFile("./.zsign_debug/CMSSignature.der.new", strCMSSignatureSlot.data() + 8, strCMSSignatureSlot.size() - 8);
		WriteFile("./.zsign_debug/CodeSignature.blob.new", strOutput);
	}

	return true;
}

bool ZArchO::Sign(ZSignAsset *pSignAsset, bool bForce, const string &strBundleId, const string &strInfoPlistSHA1, const string &strInfoPlistSHA256, const string &strCodeResourcesData)
{
	if (NULL == m_pSignBase)
	{
		m_bEnoughSpace = false;
		ZLog::Warn(">>> Can't Find CodeSignature Segment!\n");
		return false;
	}

	string strCodeResourcesSHA1;
	string strCodeResourcesSHA256;
	if(strCodeResourcesData.empty())
	{
		strCodeResourcesSHA1.append(20, 0);
		strCodeResourcesSHA256.append(32, 0);
	}
	else
	{
		SHASum(strCodeResourcesData, strCodeResourcesSHA1, strCodeResourcesSHA256);
	}
	
	string strCodeSignBlob;
	BuildCodeSignature(pSignAsset, bForce, strBundleId, strInfoPlistSHA1, strInfoPlistSHA256, strCodeResourcesSHA1, strCodeResourcesSHA256, strCodeSignBlob);
	if (strCodeSignBlob.empty())
	{
		ZLog::Error(">>> Build CodeSignature Failed!\n");
		return false;
	}

	int nSpaceLength = (int)m_uLength - (int)m_uCodeLength - (int)strCodeSignBlob.size();
	if (nSpaceLength < 0)
	{
		m_bEnoughSpace = false;
		ZLog::WarnV(">>> No Enough CodeSignature Space! Length => Now: %d, Need: %d\n", (int)m_uLength - (int)m_uCodeLength, (int)strCodeSignBlob.size());
		return false;
	}

	memcpy(m_pBase + m_uCodeLength, strCodeSignBlob.data(), strCodeSignBlob.size());
	//memset(m_pBase + m_uCodeLength + strCodeSignBlob.size(), 0, nSpaceLength);
	return true;
}

uint32_t ZArchO::ReallocCodeSignSpace(const string &strNewFile)
{
	RemoveFile(strNewFile.c_str());

	uint32_t uNewLength = m_uCodeLength + ByteAlign(((m_uCodeLength / 4096) + 1) * (20 + 32), 4096) + 16384; //16K May Be Enough
	if (NULL == m_pLinkEditSegment || uNewLength <= m_uLength)
	{
		return 0;
	}

	load_command *pseglc = (load_command *)m_pLinkEditSegment;
	switch (BO(pseglc->cmd))
	{
	case LC_SEGMENT:
	{
		segment_command *seglc = (segment_command *)m_pLinkEditSegment;
		seglc->vmsize = ByteAlign(BO(seglc->vmsize) + (uNewLength - m_uLength), 4096);
		seglc->vmsize = BO(seglc->vmsize);
		seglc->filesize = uNewLength - BO(seglc->fileoff);
		seglc->filesize = BO(seglc->filesize);
	}
	break;
	case LC_SEGMENT_64:
	{
		segment_command_64 *seglc = (segment_command_64 *)m_pLinkEditSegment;
		seglc->vmsize = ByteAlign(BO(seglc->vmsize) + (uNewLength - m_uLength), 4096);
		seglc->vmsize = BO(seglc->vmsize);
		seglc->filesize = uNewLength - BO(seglc->fileoff);
		seglc->filesize = BO(seglc->filesize);
	}
	break;
	}

	codesignature_command *pcslc = (codesignature_command *)m_pCodeSignSegment;
	if (NULL == pcslc)
	{
		if (m_uLoadCommandsFreeSpace < 4)
		{
			ZLog::Error(">>> Can't Find Free Space Of LoadCommands For CodeSignature!\n");
			return 0;
		}

		pcslc = (codesignature_command *)(m_pBase + m_uHeaderSize + BO(m_pHeader->sizeofcmds));
		pcslc->cmd = BO(LC_CODE_SIGNATURE);
		pcslc->cmdsize = BO((uint32_t)sizeof(codesignature_command));
		pcslc->dataoff = BO(m_uCodeLength);
		m_pHeader->ncmds = BO(BO(m_pHeader->ncmds) + 1);
		m_pHeader->sizeofcmds = BO(BO(m_pHeader->sizeofcmds) + sizeof(codesignature_command));
	}
	pcslc->datasize = BO(uNewLength - m_uCodeLength);

	if (!AppendFile(strNewFile.c_str(), (const char *)m_pBase, m_uLength))
	{
		return 0;
	}

	string strPadding;
	strPadding.append(uNewLength - m_uLength, 0);
	if (!AppendFile(strNewFile.c_str(), strPadding))
	{
		RemoveFile(strNewFile.c_str());
		return 0;
	}

	return uNewLength;
}

bool ZArchO::InjectDyLib(bool bWeakInject, const char *szDyLibPath, bool &bCreate)
{
	if (NULL == m_pHeader)
	{
		return false;
	}

	uint8_t *pLoadCommand = m_pBase + m_uHeaderSize;
	for (uint32_t i = 0; i < BO(m_pHeader->ncmds); i++)
	{
		load_command *plc = (load_command *)pLoadCommand;
		uint32_t uLoadType = BO(plc->cmd);
		if (LC_LOAD_DYLIB == uLoadType || LC_LOAD_WEAK_DYLIB == uLoadType)
		{
			dylib_command *dlc = (dylib_command *)pLoadCommand;
			const char *szDyLib = (const char *)(pLoadCommand + BO(dlc->dylib.name.offset));
			if (0 == strcmp(szDyLib, szDyLibPath))
			{	
				if((bWeakInject && (LC_LOAD_WEAK_DYLIB != uLoadType)) || (!bWeakInject && (LC_LOAD_DYLIB != uLoadType)))
				{
					dlc->cmd = BO((uint32_t)(bWeakInject ? LC_LOAD_WEAK_DYLIB : LC_LOAD_DYLIB));
					ZLog::WarnV(">>> DyLib Load Type Changed! %s -> %s\n", (LC_LOAD_DYLIB == uLoadType) ? "LC_LOAD_DYLIB" : "LC_LOAD_WEAK_DYLIB", bWeakInject ? "LC_LOAD_WEAK_DYLIB" : "LC_LOAD_DYLIB");
				}
				else
				{
					ZLog::WarnV(">>> DyLib Is Already Existed! %s\n");
				}
				return true;
			}
		}
		pLoadCommand += BO(plc->cmdsize);
	}

	uint32_t uDylibPathLength = strlen(szDyLibPath);
	uint32_t uDylibPathPadding = (8 - uDylibPathLength % 8);
	uint32_t uDyLibCommandSize = sizeof(dylib_command) + uDylibPathLength + uDylibPathPadding;
	if (m_uLoadCommandsFreeSpace > 0 && m_uLoadCommandsFreeSpace < uDyLibCommandSize) // some bin doesn't have '__text'
	{
		ZLog::Error(">>> Can't Find Free Space Of LoadCommands For LC_LOAD_DYLIB Or LC_LOAD_WEAK_DYLIB!\n");
		return false;
	}

	//add
	dylib_command *dlc = (dylib_command *)(m_pBase + m_uHeaderSize + BO(m_pHeader->sizeofcmds));
	dlc->cmd = BO((uint32_t)(bWeakInject ? LC_LOAD_WEAK_DYLIB : LC_LOAD_DYLIB));
	dlc->cmdsize = BO(uDyLibCommandSize);
	dlc->dylib.name.offset = BO((uint32_t)sizeof(dylib_command));
	dlc->dylib.timestamp = BO((uint32_t)2);
	dlc->dylib.current_version = 0;
	dlc->dylib.compatibility_version = 0;

	string strDylibPath = szDyLibPath;
	strDylibPath.append(uDylibPathPadding, 0);

	uint8_t *pDyLibPath = (uint8_t *)dlc + sizeof(dylib_command);
	memcpy(pDyLibPath, strDylibPath.data(), uDylibPathLength + uDylibPathPadding);

	m_pHeader->ncmds = BO(BO(m_pHeader->ncmds) + 1);
	m_pHeader->sizeofcmds = BO(BO(m_pHeader->sizeofcmds) + uDyLibCommandSize);

	bCreate = true;
	return true;
}
