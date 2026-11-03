#nullable enable
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Runtime.Loader;

static class Test {
  public record Item(int Key,int Order);
  public struct Value { public int Key,Order; }
  static void Check(bool ok) { if(!ok)throw new Exception("Sort regression"); }
  static int Key(object? v) => v switch {int x=>x, string s=>int.Parse(s), Item x=>x.Key, Value x=>x.Key, null=>-1,_=>throw new Exception()};
  static (object?[],int) Sort<T>(Assembly assembly,object?[] input,bool reverse) {
    var type=assembly.GetType("BetterList`1",true)!.MakeGenericType(typeof(T));
    var list=Activator.CreateInstance(type)!;
    // Spare capacity contains sentinels, which must never participate in sorting.
    var buffer=new T[input.Length+7];
    for(int i=0;i<input.Length;i++)buffer[i]=(T)input[i]!;
    type.GetField("buffer")!.SetValue(list,buffer);type.GetField("size")!.SetValue(list,input.Length);
    var before=(T[])buffer.Clone();int comparisons=0;
    Comparison<T> compare=(a,b)=>{comparisons++;return (reverse?-1:1)*Key(a).CompareTo(Key(b));};
    type.GetMethod("Sort")!.Invoke(list,new object[]{compare});
    Check(ReferenceEquals(buffer,type.GetField("buffer")!.GetValue(list)));
    Check((int)type.GetField("size")!.GetValue(list)! ==input.Length);
    for(int i=input.Length;i<buffer.Length;i++)Check(Equals(before[i],buffer[i]));
    return (buffer.Take(input.Length).Cast<object?>().ToArray(),comparisons);
  }
  static void Cases<T>(Assembly original,Assembly patched,Func<int,int,object?> create) {
    var rng=new Random(42);
    foreach(int n in new[]{0,1,2,8,32,128,512})foreach(bool reverse in new[]{false,true}) {
      var cases=new List<int[]> {
        Enumerable.Range(0,n).ToArray(),Enumerable.Range(0,n).Reverse().ToArray(),
        Enumerable.Repeat(2,n).ToArray(),Enumerable.Range(0,n).Select(_=>rng.Next(12)).ToArray(),
        Enumerable.Range(1,Math.Max(0,n-1)).Append(0).Take(n).ToArray()
      };
      for(int c=0;c<20;c++)cases.Add(Enumerable.Range(0,n).Select(_=>rng.Next(Math.Max(2,n))).ToArray());
      foreach(var keys in cases) {
        var input=keys.Select((key,i)=>create(key,i)).ToArray();
        var a=Sort<T>(original,input,reverse);var b=Sort<T>(patched,input,reverse);
        Check(a.Item1.SequenceEqual(b.Item1));
      }
    }
  }
  static void Main(string[] args) {
    byte[] bytes=File.ReadAllBytes(args[0]),expected=File.ReadAllBytes(args[1]),replacement=File.ReadAllBytes(args[2]);
    using var pe=new PEReader(new MemoryStream(bytes));var md=pe.GetMetadataReader();
    var method=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x1f6));var body=pe.GetMethodBody(method.RelativeVirtualAddress);
    Check(md.GetString(method.Name)=="Sort" && md.GetString(md.GetTypeDefinition(method.GetDeclaringType()).Name)=="BetterList`1");
    Check(md.GetGuid(md.GetModuleDefinition().Mvid)==Guid.Parse("4900A8E6-EA01-4E8A-AE66-036836545DD2"));
    Check(body.MaxStack==11 && body.ExceptionRegions.Length==0 && body.GetILBytes()!.SequenceEqual(expected));
    Check(Convert.ToHexString(md.GetBlobBytes(md.GetStandaloneSignature(body.LocalSignature).Signature))=="070302081300");
    var section=pe.PEHeaders.SectionHeaders.First(s=>method.RelativeVirtualAddress>=s.VirtualAddress && method.RelativeVirtualAddress<s.VirtualAddress+s.VirtualSize);
    int header=method.RelativeVirtualAddress-section.VirtualAddress+section.PointerToRawData;
    int code=header+(BitConverter.ToUInt16(bytes,header)>>12)*4;
    Check(expected.Length==replacement.Length);
    byte[] changed=(byte[])bytes.Clone();replacement.CopyTo(changed,code);
    for(int i=0;i<bytes.Length;i++)if(i<code||i>=code+expected.Length)Check(bytes[i]==changed[i]);
    var contexts=new[]{new AssemblyLoadContext("original",true),new AssemblyLoadContext("patched",true)};
    foreach(var c in contexts)c.Resolving+=(ctx,name)=>name.Name==typeof(Test).Assembly.GetName().Name?typeof(Test).Assembly:null;
    var original=contexts[0].LoadFromStream(new MemoryStream(bytes));var patched=contexts[1].LoadFromStream(new MemoryStream(changed));
    foreach(var assembly in new[]{original,patched}) {
      var type=assembly.GetType("BetterList`1",true)!.MakeGenericType(typeof(int));
      foreach(int count in new[]{0,1}) {
        var list=Activator.CreateInstance(type)!;
        if(count==1)type.GetField("buffer")!.SetValue(list,new[]{42});
        type.GetField("size")!.SetValue(list,count);
        type.GetMethod("Sort")!.Invoke(list,new object?[]{null});
      }
    }
    Cases<int>(original,patched,(k,i)=>k);Cases<string>(original,patched,(k,i)=>k==0?null:k.ToString());
    Cases<Item>(original,patched,(k,i)=>new Item(k,i));Cases<Value>(original,patched,(k,i)=>new Value{Key=k,Order=i});
    int n=1024;var late=Enumerable.Range(1,n-1).Append(0).Cast<object?>().ToArray();
    var a=Sort<int>(original,late,false);var b=Sort<int>(patched,late,false);
    Check(a.Item1.SequenceEqual(b.Item1) && b.Item2<4*n && a.Item2>n*n/2);
    var descending=Enumerable.Range(0,n).Reverse().Cast<object?>().ToArray();
    var worstBefore=Sort<int>(original,descending,false);var worstAfter=Sort<int>(patched,descending,false);
    Check(worstAfter.Item2<=worstBefore.Item2+n);
    Console.WriteLine($"PASS: 1400 original/patched sorts; values/references/nulls, stable ties, capacity/size/identity, exact APK IL/locals; late icon comparisons {a.Item2} -> {b.Item2}");
  }
}
