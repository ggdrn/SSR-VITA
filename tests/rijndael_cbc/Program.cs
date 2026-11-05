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
    var cbcMethod=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0xaaf));
    var cbcBody=pe.GetMethodBody(cbcMethod.RelativeVirtualAddress);
    var cbcCode=cbcBody.GetILBytes()!;
    if(md.GetString(cbcMethod.Name)!="CBC" || !cbcCode.SequenceEqual(File.ReadAllBytes(args[4])))throw new Exception("CBC differs");
    var ecbMethod=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x38b1));
    if(!pe.GetMethodBody(ecbMethod.RelativeVirtualAddress).GetILBytes()!.SequenceEqual(File.ReadAllBytes(args[5])))throw new Exception("ECB differs");
    foreach(var methodHandle in new[]{MetadataTokens.MethodDefinitionHandle(0xaaf),MetadataTokens.MethodDefinitionHandle(0x38b1)})
      if(Convert.ToHexString(md.GetBlobBytes(md.GetMethodDefinition(methodHandle).Signature))!="2002011D051D05")throw new Exception("CBC/ECB signature differs");
    var cbcDynamic=new DynamicMethod("OriginalCbc",typeof(void),new[]{typeof(CbcState),typeof(byte[]),typeof(byte[])},typeof(Fixture).Module,true);
    var cbcInfo=cbcDynamic.GetDynamicILInfo();
    cbcInfo.SetLocalSignature(md.GetBlobBytes(md.GetStandaloneSignature(cbcBody.LocalSignature).Signature));
    for(int at=0;at<cbcCode.Length;) {
      ushort op=cbcCode[at++];if(op==0xfe)op=(ushort)(0xfe00|cbcCode[at++]);var o=ops[op];
      int n=o.OperandType switch {OperandType.InlineNone=>0,OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,_=>4};
      if(o.OperandType==OperandType.InlineField) {
        var f=md.GetFieldDefinition(MetadataTokens.FieldDefinitionHandle(BitConverter.ToInt32(cbcCode,at)&0xffffff));
        BitConverter.GetBytes(cbcInfo.GetTokenFor(typeof(CbcState).GetField(md.GetString(f.Name))!.FieldHandle)).CopyTo(cbcCode,at);
      } else if(o.OperandType==OperandType.InlineMethod) {
        int token=BitConverter.ToInt32(cbcCode,at);
        MethodInfo target=token switch {0x06000aae=>typeof(CbcState).GetMethod("ECB")!,0x06000dba=>typeof(Buffer).GetMethod("BlockCopy")!,_=>throw new Exception("Unexpected CBC call")};
        BitConverter.GetBytes(cbcInfo.GetTokenFor(target.MethodHandle)).CopyTo(cbcCode,at);
      } else if(o.OperandType==OperandType.InlineType) {
        if(BitConverter.ToInt32(cbcCode,at)!=0x02000013)throw new Exception("Unexpected CBC type");
        BitConverter.GetBytes(cbcInfo.GetTokenFor(typeof(byte).TypeHandle)).CopyTo(cbcCode,at);
      }
      at+=n;
    }
    cbcInfo.SetCode(cbcCode,cbcBody.MaxStack);
    var cbc=cbcDynamic.CreateDelegate<Action<CbcState,byte[],byte[]>>();
    CbcState.block=original;
    var rng=new Random(860086);using var w=new BinaryWriter(File.Create(args[2]));
    var state=new CbcState();
    for(int test=0;test<4096;test++) {
      uint Next(){return (uint)rng.NextInt64(0,1L<<32);}
      if(test%16==0) {
        foreach(var table in new[]{iT0,iT1,iT2,iT3})for(int j=0;j<256;j++)table[j]=Next();
        rng.NextBytes(iSBox);state.keys=Enumerable.Range(0,120).Select(_=>Next()).ToArray();
        rng.NextBytes(state.temp);rng.NextBytes(state.temp2);
      }
      var input=new byte[40];var output=new byte[40];rng.NextBytes(input);rng.NextBytes(output);
      bool inplace=test%2==0;if(inplace)output=input;
      w.Write(inplace?1u:0u);w.Write(input);w.Write(output);w.Write(state.temp);w.Write(state.temp2);
      cbc(state,input,output);
      w.Write(output);w.Write(state.temp);w.Write(state.temp2);
      foreach(var k in state.keys)w.Write(k);
      foreach(var table in new[]{iT0,iT1,iT2,iT3})foreach(var x in table)w.Write(x);w.Write(iSBox);
    }
    Console.WriteLine("PASS: original CBC and Decrypt256 IL executed: 4096 chained blocks, IV/scratch state, in-place/separate buffers and untouched tails");
  }
}
public class CbcState {
  public bool encrypt=false;public int BlockSizeByte=32;
  public byte[] temp=new byte[40],temp2=new byte[40];public uint[] keys=Array.Empty<uint>();
  public static Fixture.Block block=null!;
  public virtual void ECB(byte[] input,byte[] output)=>block(this,input,output,keys);
}
