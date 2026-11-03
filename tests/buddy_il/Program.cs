// Metadata only: do not load or execute the game's assemblies.
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text.Json;
using var pe = new PEReader(File.OpenRead(args[0]));
var md = pe.GetMetadataReader();
var method = md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x443d));
var klass = md.GetTypeDefinition(method.GetDeclaringType());
if(md.GetString(klass.Name) != "ui_chao_set_cell" || md.GetString(method.Name) != "Update")
    throw new Exception("Unsupported APK method");
Console.WriteLine(JsonSerializer.Serialize(new {
    mvid=md.GetGuid(md.GetModuleDefinition().Mvid).ToString(),
    il=Convert.ToBase64String(pe.GetMethodBody(method.RelativeVirtualAddress).GetILBytes()!),
    fields=klass.GetFields().ToDictionary(h=>MetadataTokens.GetToken(h).ToString(),h=>md.GetString(md.GetFieldDefinition(h).Name))
}));
