using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

public static class Fixture {
  public static uint[] iT0=new uint[256],iT1=new uint[256],iT2=new uint[256],iT3=new uint[256];
  public static byte[] iSBox=new byte[256];
  public delegate void Block(object self,byte[] input,byte[] output,uint[] keys);
  public static void Main(string[] args) {
    using var pe=new PEReader(File.OpenRead(args[0]));var md=pe.GetMetadataReader();
    var method=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x38b8));
    var type=md.GetTypeDefinition(method.GetDeclaringType());
    if(md.GetString(type.Namespace)!=args[3] || md.GetString(type.Name)!="RijndaelTransform" ||
       Convert.ToHexString(md.GetBlobBytes(method.Signature))!="2003011D051D051D09")
      throw new Exception("Production lookup namespace/type/signature differs from APK metadata");
    var body=pe.GetMethodBody(method.RelativeVirtualAddress);var il=body.GetILBytes()!;
    if(md.GetGuid(md.GetModuleDefinition().Mvid)!=Guid.Parse("f5d258d5-2c9d-4f52-8ab3-f099f3055e87") ||
       md.GetString(method.Name)!="Decrypt256" || body.ExceptionRegions.Length!=0 ||
       !il.SequenceEqual(File.ReadAllBytes(args[1])))throw new Exception("Unsupported mscorlib IL");
    // Execute the actual original IL, relocating only its five static table tokens.
    var dynamic=new DynamicMethod("Original",typeof(void),new[]{typeof(object),typeof(byte[]),typeof(byte[]),typeof(uint[])},typeof(Fixture).Module,true);
    var info=dynamic.GetDynamicILInfo();
    info.SetLocalSignature(md.GetBlobBytes(md.GetStandaloneSignature(body.LocalSignature).Signature));
    var ops=typeof(OpCodes).GetFields().Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>(ushort)o.Value);
    for(int at=0;at<il.Length;) {
      ushort op=il[at++];if(op==0xfe)op=(ushort)(0xfe00|il[at++]);var o=ops[op];
      int size=o.OperandType switch {OperandType.InlineNone=>0,OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,OperandType.InlineField=>4,_=>throw new Exception("Unexpected opcode "+o.Name)};
      if(o==OpCodes.Ldsfld) {
        var field=md.GetFieldDefinition(MetadataTokens.FieldDefinitionHandle(BitConverter.ToInt32(il,at)&0xffffff));
        int token=info.GetTokenFor(typeof(Fixture).GetField(md.GetString(field.Name))!.FieldHandle);
        BitConverter.GetBytes(token).CopyTo(il,at);
      }
      at+=size;
    }
    info.SetCode(il,body.MaxStack);var original=dynamic.CreateDelegate<Block>();
    var rng=new Random(670068);using var w=new BinaryWriter(File.Create(args[2]));
    for(int test=0;test<4096;test++) {
      uint Next(){return (uint)rng.NextInt64(0,1L<<32);}
      // Arbitrary tables and expanded keys make accidental matching unlikely.
      foreach(var table in new[]{iT0,iT1,iT2,iT3})for(int j=0;j<256;j++)table[j]=Next();
      rng.NextBytes(iSBox);var keys=Enumerable.Range(0,120).Select(_=>Next()).ToArray();
      var input=new byte[32];rng.NextBytes(input);var output=new byte[32];
      original(null!,input,output,keys);
      w.Write(input);w.Write(output);foreach(var k in keys)w.Write(k);
      foreach(var table in new[]{iT0,iT1,iT2,iT3})foreach(var x in table)w.Write(x);w.Write(iSBox);
      var inplace=(byte[])input.Clone();original(null!,inplace,inplace,keys);
      if(!inplace.SequenceEqual(output))throw new Exception("Original alias mismatch");
    }
    Console.WriteLine("PASS: original 6490-byte Mono IL executed for 4096 reference blocks and in-place checks");
  }
}
