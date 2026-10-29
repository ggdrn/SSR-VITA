using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Reflection.Emit;
using System.Text.Json;
using var pe=new PEReader(File.OpenRead(args[0]));var md=pe.GetMetadataReader();
var ops=typeof(OpCodes).GetFields().Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>unchecked((ushort)o.Value));
string TypeName(EntityHandle h){
 if(h.Kind==HandleKind.TypeReference){var t=md.GetTypeReference((TypeReferenceHandle)h);return t.ResolutionScope.Kind==HandleKind.TypeReference?TypeName(t.ResolutionScope)+"/"+md.GetString(t.Name):md.GetString(t.Namespace)+"."+md.GetString(t.Name);}
 return "?";
}
bool ListEnum(int token){try{var h=MetadataTokens.EntityHandle(token);if(h.Kind!=HandleKind.TypeSpecification)return false;var b=md.GetBlobReader(md.GetTypeSpecification((TypeSpecificationHandle)h).Signature);if(b.ReadByte()!=0x15||b.ReadByte()!=0x11)return false;int c=b.ReadCompressedInteger();return (c&3)==1&&TypeName(MetadataTokens.EntityHandle(0x01000000|(c>>2)))=="System.Collections.Generic.List`1/Enumerator";}catch{return false;}}
bool Dispose(int token){try{var h=MetadataTokens.EntityHandle(token);if(h.Kind!=HandleKind.MemberReference)return false;var m=md.GetMemberReference((MemberReferenceHandle)h);return md.GetString(m.Name)=="Dispose"&&TypeName(m.Parent)=="System.IDisposable";}catch{return false;}}
var result=new List<object>();
foreach(var mh in md.MethodDefinitions){var m=md.GetMethodDefinition(mh);string name=md.GetString(m.Name);if(name!="Update"&&name!="LateUpdate"&&name!="FixedUpdate")continue;
 var type=md.GetTypeDefinition(m.GetDeclaringType());if(!type.GetDeclaringType().IsNil||m.RelativeVirtualAddress==0)continue;
 var sig=md.GetBlobReader(m.Signature);var sh=sig.ReadSignatureHeader();if(sh.IsGeneric)sig.ReadCompressedInteger();if(sig.ReadCompressedInteger()!=0)continue;
 var body=pe.GetMethodBody(m.RelativeVirtualAddress);var bytes=body.GetILBytes()!;var spans=new List<int[]>();var targets=new HashSet<int>();
 var ins=new List<(int at,ushort op,int operand,int end)>();
 for(int i=0;i<bytes.Length;){int at=i;ushort op=bytes[i++];if(op==254)op=(ushort)(0xfe00|bytes[i++]);var o=ops[op];int n=o.OperandType switch{OperandType.InlineNone=>0,OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,OperandType.InlineVar=>2,OperandType.InlineI8 or OperandType.InlineR=>8,OperandType.InlineSwitch=>4+4*BitConverter.ToInt32(bytes,i),_=>4};int operand=n==4?BitConverter.ToInt32(bytes,i):0;int end=i+n;ins.Add((at,op,operand,end));
 if(o.OperandType==OperandType.ShortInlineBrTarget)targets.Add(end+(sbyte)bytes[i]);if(o.OperandType==OperandType.InlineBrTarget)targets.Add(end+operand);
 if(o.OperandType==OperandType.InlineSwitch)for(int k=0;k<BitConverter.ToInt32(bytes,i);k++)targets.Add(end+BitConverter.ToInt32(bytes,i+4+4*k));i=end;}
 foreach(var eh in body.ExceptionRegions){targets.Add(eh.TryOffset);targets.Add(eh.TryOffset+eh.TryLength);targets.Add(eh.HandlerOffset);targets.Add(eh.HandlerOffset+eh.HandlerLength);if(eh.Kind==ExceptionRegionKind.Filter)targets.Add(eh.FilterOffset);}
 for(int i=0;i+3<ins.Count;i++){var a=ins[i];if(!(a.op>=6&&a.op<=9||a.op==0x11||a.op==0xfe0c))continue;var b=ins[i+1];var c=ins[i+2];var d=ins[i+3];if(b.op!=0x8c||c.op!=0x6f||d.op!=0xdc||!ListEnum(b.operand)||!Dispose(c.operand))continue;if(targets.Any(t=>t>a.at&&t<c.end))continue;spans.Add(new[]{a.at,c.end-a.at});}
 if(spans.Count>0)result.Add(new{ns=md.GetString(type.Namespace),type=md.GetString(type.Name),method=name,hex=Convert.ToHexString(bytes),spans});
}
Console.WriteLine(JsonSerializer.Serialize(result,new JsonSerializerOptions{WriteIndented=true}));
