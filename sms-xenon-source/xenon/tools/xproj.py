#!/usr/bin/env python3
"""Write the Xbox 360 MSBuild project (VS2010 + XDK) for the staged tree.

usage: xproj.py REPO STAGE      writes STAGE/xenon/sms.vcxproj

Units: the game's (compile_commands.json, as xprobe takes them), the port's own
(xenon/units-port.txt), the GX core (xenon/units-gx.txt) and xenon/platform, xenon/gx.
Each group gets the flags its CMake target has; every unit /fp:precise (the port's
-ffp-contract=off) except xe_fma.cpp (xenon/platform/xe_fma.cpp says why)."""
import json, os, sys
from xml.sax.saxutils import escape

repo, stage = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(repo, 'build/linux-32/compile_commands.json')))

def listed(name):
    return [l.strip() for l in open(os.path.join(repo, 'xenon', name)) if l.strip() and not l.startswith('#')]

port360 = set(listed('units-port.txt'))
gx360 = set(listed('units-gx.txt'))
game, port = [], []
for e in cc:
    f = os.path.relpath(e['file'], repo); c = e['command']
    if '_vkdeps' in f or '/vk/' in f or f.startswith('ext/multiplayer/'):
        continue
    is_game = '-DVERSION_GMSE01' in c and ('-std=gnu++03' in c or '-std=gnu99' in c)
    for a, b in (('build/linux-32/patched/', 'game/'), ('decomp/', 'game/')):
        if f.startswith(a):
            f = b + f[len(a):]; break
    if is_game:
        game.append((f, 'SMS.pch' in c))
    elif f in port360 or f in gx360:
        port.append(f)
own = []
for d in ('xenon/platform', 'xenon/gx'):
    p = os.path.join(stage, d)
    if os.path.isdir(p):
        own += [d + '/' + f for f in sorted(os.listdir(p)) if f.endswith(('.cpp', '.c'))]

libs = ['dolphin', 'JSystem', 'THPPlayer', 'PowerPC_EABI_Support', 'TRK_MINNOW_DOLPHIN', 'OdemuExi2']
inc = ['xenon\\msvc\\include', 'platform\\gx\\port_include', 'platform\\gx\\include', 'src\\port_include', 'src',
       'platform', 'game\\include'] + ['game\\libs\\%s\\include' % l for l in libs] + \
      ['sdk\\include', 'ext', 'platform\\mods\\include', 'ext-generated']
inc = ['$(MSBuildProjectDirectory)\\..\\' + i for i in inc]
prelude = '$(MSBuildProjectDirectory)\\msvc\\xe_prelude.h'
compat = '$(MSBuildProjectDirectory)\\..\\src\\port_compat.h'
pch = '$(MSBuildProjectDirectory)\\..\\game\\include\\SMS.pch'

def unit(path, forced, defs, fp='Precise', lang='CompileAsCpp'):
    rel = '..\\' + path.replace('/', '\\')
    objdir = '$(IntDir)' + os.path.dirname(path).replace('/', '\\') + '\\'
    return ('    <ClCompile Include="%s"><CompileAs>%s</CompileAs><ForcedIncludeFiles>%s</ForcedIncludeFiles>'
            '<PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>'
            '<FloatingPointModel>%s</FloatingPointModel><ObjectFileName>%s</ObjectFileName></ClCompile>'
            % (escape(rel), lang, escape(';'.join(forced)), escape(';'.join(defs)), fp, escape(objdir)))

items = []
game_objs = []
def objpath(path):
    return 'out\\$(Configuration)\\obj\\' + os.path.dirname(path).replace('/', '\\') + '\\' + os.path.splitext(os.path.basename(path))[0] + '.obj'
for f, has_pch in sorted(set(game)):
    items.append(unit(f, [prelude, compat] + ([pch] if has_pch else []), ['GEKKO', 'VERSION_GMSE01']))
    game_objs.append(objpath(f))
for f in sorted(set(port)):
    # msl_rand.c defines C symbols the game declares extern "C", so it builds as C (C89 here);
    # msl_math.c is C99 and builds as C++, its functions reached through its own C++ declarations;
    # GD is C that leans on void* conversions, so C as well
    forced = [prelude] + (['$(MSBuildProjectDirectory)\\..\\platform\\gx\\src\\gd_host_prefix.h'] if '/src/gd/' in f else [])
    items.append(unit(f, forced, ['GEKKO', 'VERSION_GMSE01', 'SMS_AUDIO_DSP_HLE=1', 'SMS_HAVE_GAME_BUILD_H=1'],
                      lang='CompileAsC' if f.endswith('msl_rand.c') or '/src/gd/' in f else 'CompileAsCpp'))
gamedir = os.path.join(stage, 'xenon/game')
if os.path.isdir(gamedir):
    for f in sorted(os.listdir(gamedir)):
        if f.endswith('.cpp'):
            items.append(unit('xenon/game/' + f, [prelude, compat], ['GEKKO', 'VERSION_GMSE01']))
            game_objs.append(objpath('xenon/game/' + f))
for f in own:
    items.append(unit(f, [prelude], ['GEKKO', 'VERSION_GMSE01', 'SMS_AUDIO_DSP_HLE=1'],
                      'Fast' if f.endswith('xe_fma.cpp') else 'Precise'))

proj = '''<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|Xbox 360"><Configuration>Release</Configuration><Platform>Xbox 360</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Master|Xbox 360"><Configuration>Master</Configuration><Platform>Xbox 360</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{8C1D5B70-3F2A-4E6B-9D41-2A7F0C5E9B33}</ProjectGuid><Keyword>Xbox360Proj</Keyword></PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <CharacterSet>MultiByte</CharacterSet>
    <PlatformToolset>2010-01</PlatformToolset>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <PropertyGroup>
    <OutDir>$(MSBuildProjectDirectory)\\out\\$(Configuration)\\</OutDir>
    <IntDir>$(MSBuildProjectDirectory)\\out\\$(Configuration)\\obj\\</IntDir>
    <TargetName>sms</TargetName>
    <OutputFile>$(OutDir)$(TargetName)$(TargetExt)</OutputFile>
    <ImageXexOutput>$(OutDir)default.xex</ImageXexOutput>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>%(inc)s</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>NDEBUG=1;_XBOX;TARGET_PC;TARGET_X360;BUILD_VERSION=2</PreprocessorDefinitions>
      <WarningLevel>Level1</WarningLevel>
      <DisableSpecificWarnings>4996;4244;4305;4005;4800;4018;4101;4102;4146</DisableSpecificWarnings>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <Optimization>Full</Optimization>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <StringPooling>true</StringPooling>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <ExceptionHandling>Sync</ExceptionHandling>
      <!-- the XDK defaults to /GR-: vtables without RTTI, and the port's dynamic_casts (EventWatcher,
           ext/) then read a null locator -->
      <RuntimeTypeInfo>true</RuntimeTypeInfo>
      <!-- /GS- : no stack cookie in every function with an array (the console's CPU is the limit) -->
      <BufferSecurityCheck>false</BufferSecurityCheck>
      <AdditionalOptions>/bigobj %%(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
    <Link>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <SetChecksum>true</SetChecksum>
      <OptimizeReferences>true</OptimizeReferences>
      <StackCommitSize>1048576</StackCommitSize>
      <AdditionalDependencies>xapilib.lib;d3d9.lib;d3dx9.lib;xgraphics.lib;xaudio2.lib;x3daudio.lib;xmcore.lib;xboxkrnl.lib;xnet.lib</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <!-- Master: what ships (xenon/tools/xpackage): Release with link-time code generation -->
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Master'">
    <ClCompile>
      <PreprocessorDefinitions>MASTER;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <WholeProgramOptimization>true</WholeProgramOptimization>
      <!-- the XDK's own: /Ou schedules before register allocation (in-order PowerPC), /Oc drops the
           trap around each integer divide (the GameCube's code never trapped on one either) -->
      <AdditionalOptions>/Ou /Oc /bigobj %%(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
    <Link><LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration></Link>
  </ItemDefinitionGroup>
  <ItemGroup>
%(items)s
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <!-- the game's operator new/delete stay the game's (xe_rename_new.ps1) -->
  <Target Name="XeRenameGameNew" AfterTargets="ClCompile" BeforeTargets="Link">
    <WriteLinesToFile File="$(IntDir)game_objs.txt" Lines="%(game_objs)s" Overwrite="true" />
    <Exec WorkingDirectory="$(MSBuildProjectDirectory)" Command="powershell -NoProfile -ExecutionPolicy Bypass -File msvc\\xe_rename_new.ps1 -List &quot;$(IntDir)game_objs.txt&quot;" />
  </Target>
</Project>
''' % {'inc': escape(';'.join(inc)), 'items': '\n'.join(items), 'game_objs': escape(';'.join(o.replace('$(Configuration)', '$(Configuration)') for o in game_objs))}
out = os.path.join(stage, 'xenon', 'sms.vcxproj')
open(out, 'w').write(proj.replace('\n', '\r\n'))
print('xproj: %d game, %d port, %d own units -> %s' % (len(set(game)), len(set(port)), len(own), out))
