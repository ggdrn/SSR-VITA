// Offline metadata only: no game assemblies are executed or rewritten.
// dotnet run --project scripts/race_warmup_gen -- Assembly-CSharp.dll src/util/race_warmup.h
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using var pe = new PEReader(File.OpenRead(args[0]));
var md = pe.GetMetadataReader();
string[] prefixes = { "Obj", "Character", "Chao", "State", "Stage", "Movement", "ResPath", "Laser", "Drill", "Collision", "PlayerInformation", "GameModeStage", "HudCockpit", "HudScore", "HudRing", "HudCombo", "TerrainPlacementManager", "TenseEffectTable", "BossTypeUtil", "BossState", "TenseParameter", "GameResult", "HudResult", "HudContinue", "HudCharacter", "GameReady", "GameStart", "MultiSet", "Msg", "RankingDataContainer", "ServerGameResults", "GlowUp", "HudInterpolate" };
bool Open(TypeDefinitionHandle h) {
    var t = md.GetTypeDefinition(h);
    return t.GetGenericParameters().Count != 0 || (!t.GetDeclaringType().IsNil && Open(t.GetDeclaringType()));
}
string RootName(TypeDefinitionHandle h) {
    var t = md.GetTypeDefinition(h);
    return t.GetDeclaringType().IsNil ? md.GetString(t.Name) : RootName(t.GetDeclaringType());
}
var lines = new List<string>();
foreach (var th in md.TypeDefinitions) {
    var t = md.GetTypeDefinition(th);
    if (Open(th) || !prefixes.Any(p => RootName(th).StartsWith(p, StringComparison.Ordinal))) continue;
    foreach (var mh in t.GetMethods()) {
        var m = md.GetMethodDefinition(mh);
        if (m.RelativeVirtualAddress == 0 || m.GetGenericParameters().Count != 0 ||
            (m.Attributes & (MethodAttributes.Abstract | MethodAttributes.PinvokeImpl)) != 0 ||
            (m.ImplAttributes & MethodImplAttributes.CodeTypeMask) != MethodImplAttributes.IL ||
            md.GetString(m.Name) == ".cctor") continue;
        var bytes = pe.GetMethodBody(m.RelativeVirtualAddress).GetILBytes()!;
        lines.Add($"  {{0x{MetadataTokens.GetToken(mh):x8}, {bytes.Length}}}, // {md.GetString(t.Namespace)}.{md.GetString(t.Name)}.{md.GetString(m.Name)}");
    }
}
var guid = md.GetGuid(md.GetModuleDefinition().Mvid).ToString().ToUpperInvariant();
File.WriteAllText(args[1], "// Generated from the original APK's Assembly-CSharp metadata; see scripts/race_warmup_gen.\n" +
    "// Closed non-generic IL methods only; static initializers are excluded.\n#pragma once\n" +
    $"#define RACE_WARMUP_MVID \"{guid}\"\n" +
    "static const struct { uint32_t token, il_size; } race_warmup_methods[] = {\n" + string.Join("\n", lines) + "\n};\n");
Console.WriteLine($"{lines.Count} methods, MVID {guid}");
