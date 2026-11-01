using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Runtime.Loader;

static class Test {
  public struct V3 { public float X,Y,Z; }
  static void Check(bool value) { if(!value) throw new Exception("Trim regression"); }
  static void Exercise(Assembly assembly) {
    foreach(var element in new[]{typeof(int),typeof(long),typeof(string),typeof(object),typeof(V3)}) {
      var listType=assembly.GetType("BetterList`1",true)!.MakeGenericType(element);
      var buffer=listType.GetField("buffer")!;var size=listType.GetField("size")!;
      var trim=listType.GetMethod("Trim",BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)!;
      foreach(int capacity in new[]{0,1,4,32,127,1024}) foreach(int used in new[]{0,capacity/2,capacity}) {
        var list=Activator.CreateInstance(listType)!;var data=Array.CreateInstance(element,capacity);
        for(int i=0;i<capacity;i++) data.SetValue(element==typeof(int)?i:element==typeof(long)?(long)i:
          element==typeof(string)?"text"+i:element==typeof(object)?new object():new V3{X=i,Y=-i,Z=i+0.5f},i);
        buffer.SetValue(list,data);size.SetValue(list,used);trim.Invoke(list,null);
        var result=(Array?)buffer.GetValue(list);Check((int)size.GetValue(list)! == used);
        if(used==0) Check(result==null);
        else { Check(result!.Length==used);for(int i=0;i<used;i++)Check(Equals(result.GetValue(i),data.GetValue(i)));
          Check((used==capacity)==ReferenceEquals(result,data)); }
        trim.Invoke(list,null);Check(ReferenceEquals(result,buffer.GetValue(list)));
        // Reference copies must survive collections after the source is dropped.
        data=null!;GC.Collect();GC.WaitForPendingFinalizers();
        if(used>0)Check(((Array)buffer.GetValue(list)!).Length==used);
      }
    }
  }
  static void Main(string[] args) {
    byte[] original=File.ReadAllBytes(args[0]), expected=File.ReadAllBytes(args[1]), replacement=File.ReadAllBytes(args[2]);
    using var pe=new PEReader(new MemoryStream(original));var md=pe.GetMetadataReader();
    var method=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x1ec));var body=pe.GetMethodBody(method.RelativeVirtualAddress);
    Check(md.GetGuid(md.GetModuleDefinition().Mvid)==Guid.Parse("4900A8E6-EA01-4E8A-AE66-036836545DD2"));
    Check(body.MaxStack>=5 && body.ExceptionRegions.Length==0 && body.GetILBytes()!.SequenceEqual(expected));
    var member=md.GetMemberReference(MetadataTokens.MemberReferenceHandle(0x18));var parent=md.GetTypeReference((TypeReferenceHandle)member.Parent);
    Check(md.GetString(member.Name)=="Copy" && md.GetString(parent.Name)=="Array" && md.GetString(parent.Namespace)=="System");
    Check(Convert.ToHexString(md.GetBlobBytes(member.Signature))=="000501123D08123D0808");
    var section=pe.PEHeaders.SectionHeaders.First(s=>method.RelativeVirtualAddress>=s.VirtualAddress && method.RelativeVirtualAddress<s.VirtualAddress+s.VirtualSize);
    int header=method.RelativeVirtualAddress-section.VirtualAddress+section.PointerToRawData;
    int code=header+((original[header]&3)==2?1:(BitConverter.ToUInt16(original,header)>>12)*4);
    byte[] patched=(byte[])original.Clone();replacement.CopyTo(patched,code+0x2b);
    for(int i=0;i<patched.Length;i++)if(i<code+0x2b||i>=code+0x55)Check(patched[i]==original[i]);
    foreach(byte[] bytes in new[]{original,patched}) {
      var context=new AssemblyLoadContext(Guid.NewGuid().ToString(),true);
      context.Resolving+=(c,n)=>n.Name==typeof(Test).Assembly.GetName().Name?typeof(Test).Assembly:null;
      Exercise(context.LoadFromStream(new MemoryStream(bytes)));context.Unload();
    }
    Console.WriteLine("PASS: real APK Trim before/after, five value/reference types, 90 cases each, identity/length/content, GC, exact patch span and metadata");
  }
}
