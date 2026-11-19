# SPDX-License-Identifier: BSD-3-Clause

import copy
import os
import struct

from SCons.Environment import Environment

AllowedBootSetups = (
    ("aarch64", "efi", "system"),
    ("aarch64", "efi", "grub"),
    ("x86_64", "bios", "system"),
    ("x86_64", "bios", "grub"),
    ("x86_64", "efi", "system"),
    ("x86_64", "efi", "grub"),
    ("i686", "bios", "system"),
    ("i686", "bios", "grub"),
)


BootloaderProfiles = {
    "bios": {
        "SupportedArchitectures": [
            "i686",
            "x86_64",
        ],
        "CompilerFlags": ["-DBOOTLOADER_BIOS"],
        "AssemblerFlags": ["-DBOOTLOADER_BIOS"],
    },
    "efi": {
        "SupportedArchitectures": [
            "x86_64",
            "aarch64",
        ],
        "CompilerFlags": ["-DBOOTLOADER_EFI"],
        "AssemblerFlags": ["-DBOOTLOADER_EFI"],
    },
}

CoreFsPatchSignature = b"VLSF"
CoreFsPatchOffset = 4
CoreFsPartitionLabelOffset = 8
CoreFsPartitionUuidOffset = CoreFsPartitionLabelOffset + 32
ElToritoLoadAddress = 0x7C00
CoreFsLoadAddress = 0x90000


def GetSupportedBootTypes() -> list:
    return list(BootloaderProfiles.keys())


def ValidateBootSetup(Architecture: str, BootType: str, Bootloader: str) -> None:
    Config = (Architecture.lower(), BootType.lower(), Bootloader.lower())
    if Config not in AllowedBootSetups:
        raise ValueError(
            f"Unsupported boot setup: "
            f"Architecture={Architecture}, "
            f"BootType={BootType}, "
            f"Bootloader={Bootloader}. "
        )


def GetBootloaderBuildConfig(
    BootType: str, Architecture: str, Version: str, BuildConfig: str = "debug"
) -> dict:
    if BootType not in BootloaderProfiles:
        raise ValueError(
            f"Unsupported boot type: {BootType}. Supported: {list(BootloaderProfiles.keys())}"
        )

    Config = copy.deepcopy(BootloaderProfiles[BootType])
    SupportedArchitectures = Config.get("SupportedArchitectures", [])
    if SupportedArchitectures and Architecture not in SupportedArchitectures:
        raise ValueError(
            f"Unsupported architecture {Architecture} for boot type {BootType}. "
            f"Supported: {SupportedArchitectures}"
        )

    Config["BootType"] = BootType
    Config["Architecture"] = Architecture
    Config["Version"] = Version
    Config["BuildConfig"] = BuildConfig
    Config["OutputName"] = f"TheBootloader-{Version}_{BuildConfig}.so"

    return Config


def ConfigureBootloaderEnvironment(
    Env: Environment,
    SourcePath: str,
    ArchitecturePath: str,
    ArchitectureConfig: dict,
    BootloaderConfig: dict,
):
    Env.Append(
        ASFLAGS=ArchitectureConfig.get("AssemblyFlags", []),
        CCFLAGS=ArchitectureConfig.get("CompilerFlags", []),
        LINKFLAGS=ArchitectureConfig.get("LinkerFlags", []),
    )

    Env.Append(
        CCFLAGS=[
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-builtin",
            "-Wall",
            "-Wextra",
        ],
        CPPPATH=[
            SourcePath,
            os.path.join(SourcePath, "common"),
            "#include",
            ArchitecturePath,
        ],
        CPPDEFINES={"VALECIUM_BOOTLOADER": None},
        ASFLAGS=["-Wa,--noexecstack"],
    )

    Env.Append(CCFLAGS=BootloaderConfig.get("CompilerFlags", []))
    Env.Append(ASFLAGS=BootloaderConfig.get("AssemblerFlags", []))


def PatchBinaryValue(
    BinaryPath: str,
    Signature: bytes,
    Value: int,
    ValueFormat: str,
    ValueOffset: int = 4,
) -> None:
    if not isinstance(Signature, (bytes, bytearray)):
        raise TypeError("Signature must be bytes")

    with open(BinaryPath, "rb") as FileHandle:
        Data = bytearray(FileHandle.read())

    SignatureOffset = Data.find(Signature)
    if SignatureOffset == -1:
        raise ValueError(f"Signature {Signature!r} not found in {BinaryPath}")
    if Data.find(Signature, SignatureOffset + 1) != -1:
        raise ValueError(
            f"Signature {Signature!r} appears multiple times in {BinaryPath}"
        )

    ValueSize = struct.calcsize(ValueFormat)
    PatchOffset = SignatureOffset + ValueOffset
    if PatchOffset + ValueSize > len(Data):
        raise ValueError(f"Patch offset {PatchOffset} exceeds size of {BinaryPath}")

    struct.pack_into(ValueFormat, Data, PatchOffset, Value)

    with open(BinaryPath, "wb") as FileHandle:
        FileHandle.write(Data)


def PatchCoreFsStartAddress(
    CorePath: str,
    StartAddress: int,
) -> None:
    PatchBinaryValue(
        BinaryPath=CorePath,
        Signature=CoreFsPatchSignature,
        Value=StartAddress,
        ValueFormat="<I",
        ValueOffset=CoreFsPatchOffset,
    )


def PatchCoreFsPartitionLabel(
    CorePath: str,
    Label: bytes,
) -> None:
    if len(Label) != 32:
        raise ValueError(f"Partition label must be exactly 32 bytes, got {len(Label)}")
    PatchBinaryValue(
        BinaryPath=CorePath,
        Signature=CoreFsPatchSignature,
        Value=Label,
        ValueFormat="32s",
        ValueOffset=CoreFsPartitionLabelOffset,
    )


def PatchCoreFsPartitionUuid(
    CorePath: str,
    Uuid: bytes,
) -> None:
    if len(Uuid) != 16:
        raise ValueError(f"Partition UUID must be exactly 16 bytes, got {len(Uuid)}")
    PatchBinaryValue(
        BinaryPath=CorePath,
        Signature=CoreFsPatchSignature,
        Value=Uuid,
        ValueFormat="16s",
        ValueOffset=CoreFsPartitionUuidOffset,
    )


def ResolveCoreFsBinaryPath(
    FileSystemType: str,
    CoreFsBinaries: list,
    Stage2Path: str,
) -> str:
    TargetName = f"corefs_{FileSystemType}.bin"

    for Node in CoreFsBinaries or []:
        NodePath = Node.get_abspath() if hasattr(Node, "get_abspath") else str(Node)
        if os.path.basename(NodePath) == TargetName:
            return NodePath

    CandidatePath = os.path.join(os.path.dirname(Stage2Path), TargetName)
    if os.path.exists(CandidatePath):
        return CandidatePath

    raise FileNotFoundError(
        f"Filesystem module not found: {TargetName} (stage2 dir: {os.path.dirname(Stage2Path)})"
    )


def CreateElTorito(
    Stage1Path: str,
    Stage2Path: str,
    FileSystemType: str,
    CoreFsBinaries: list = None,
    PartitionLabel: bytes = b"\x00" * 32,
) -> str:
    if not os.path.exists(Stage1Path):
        raise FileNotFoundError(f"Stage1 bootloader file does not exist: {Stage1Path}")
    if not os.path.exists(Stage2Path):
        raise FileNotFoundError(f"Stage2 bootloader file does not exist: {Stage2Path}")

    with open(Stage1Path, "rb") as Stage1File:
        Stage1Data = Stage1File.read()

    PatchCoreFsStartAddress(Stage2Path, CoreFsLoadAddress)
    PatchCoreFsPartitionLabel(Stage2Path, PartitionLabel)

    with open(Stage2Path, "rb") as Stage2File:
        Stage2Data = Stage2File.read()

    CoreFsPath = ResolveCoreFsBinaryPath(
        FileSystemType=FileSystemType,
        CoreFsBinaries=CoreFsBinaries,
        Stage2Path=Stage2Path,
    )
    with open(CoreFsPath, "rb") as CoreFsFile:
        CoreFsData = CoreFsFile.read()

    # Pad stage1 to a full 512-byte sector so the Boot Info Table (bytes 8-31)
    # has a well-defined home and stage2 begins on a sector boundary.
    if len(Stage1Data) < 512:
        Stage1Data = Stage1Data + b"\x00" * (512 - len(Stage1Data))

    if CoreFsLoadAddress <= ElToritoLoadAddress:
        raise ValueError(
            f"Invalid corefs load address: {CoreFsLoadAddress:#x} <= {ElToritoLoadAddress:#x}"
        )

    CorefsOffset = CoreFsLoadAddress - ElToritoLoadAddress
    CorefsStart = len(Stage1Data) + len(Stage2Data)
    if CorefsStart > CorefsOffset:
        raise ValueError(
            f"core.bin exceeds corefs load boundary: {CorefsStart:#x} > {CorefsOffset:#x}"
        )

    if CorefsStart < CorefsOffset:
        Stage2Data = Stage2Data + b"\x00" * (CorefsOffset - CorefsStart)

    Combined = Stage1Data + Stage2Data + CoreFsData

    OutputPath = os.path.join(
        os.path.dirname(Stage1Path),
        os.path.basename(Stage1Path).replace(".bin", "-eltorito.bin"),
    )
    with open(OutputPath, "wb") as OutputFile:
        OutputFile.write(Combined)

    return os.path.abspath(OutputPath)
