#nullable enable
using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

class Draw { public bool dirty; public void Dirty(bool v)=>dirty=v; }
class Panel {
 public bool stat, dead; public int id; public float until, now; public Action? changed;
 public bool Clip()=>true;
 public static bool full;
}
class Items { public Widget[] data=Array.Empty<Widget>(); public int size; public Widget Item(int i)=>data[i]; }
class Widget {
 public static Items list=new(); public Panel? panel, lazyOwner; public bool enabled=true, updated;
 public Draw? draw; public int enabledCalls, ownerCalls, geometryCalls; public Action? onGeometry;
 public bool Enabled(){enabledCalls++;return enabled;}
 public Panel? Owner(){ownerCalls++;if(panel==null || panel.dead)panel=lazyOwner;return panel;}
 public bool Geometry(Panel p,bool force){geometryCalls++;onGeometry?.Invoke();return updated;}
 public Draw? DrawCall()=>draw;
}
static class Test {
 static int eqCalls;
 // Simulate native equality, including destroyed and alternate managed wrappers.
 public static bool Eq(object? a,object? b){eqCalls++;if(a is Panel pa && b is Panel pb)return pa.id==pb.id;
  if(a==null && b is Panel p)return p.dead;
  if(b==null && a is Panel q)return q.dead;
  return ReferenceEquals(a,b);}
 public static bool Ne(object? a,object? b)=>!Eq(a,b);
 static void Check(bool v){if(!v)throw new Exception("Panel regression");}
 static Delegate Make(byte[] body) {
  var method=new DynamicMethod("UpdateWidgets",typeof(void),new[]{typeof(Panel)},typeof(Test).Module,true);
  var il=method.GetDynamicILInfo();
  var locals=SignatureHelper.GetLocalVarSigHelper();
  foreach(var t in new[]{typeof(bool),typeof(bool),typeof(int),typeof(int),typeof(Widget),typeof(Draw)})locals.AddArgument(t);
  il.SetLocalSignature(locals.GetSignature());
  var fields=new Dictionary<int,(Type,string)> {
   [0x040003fd]=(typeof(Panel),"stat"),[0x0400040e]=(typeof(Panel),"until"),[0x0400040f]=(typeof(Panel),"now"),
   [0x040002a7]=(typeof(Widget),"list"),[0x0a0002f3]=(typeof(Items),"size"),[0x040002af]=(typeof(Widget),"panel"),
   [0x04000404]=(typeof(Panel),"full"),[0x040003f9]=(typeof(Panel),"changed")};
  var methods=new Dictionary<int,(Type,string)> {
   [0x06000425]=(typeof(Panel),"Clip"),[0x0a0002f2]=(typeof(Items),"Item"),[0x0a00007b]=(typeof(Widget),"Enabled"),
   [0x060002df]=(typeof(Widget),"Owner"),[0x0a000028]=(typeof(Test),"Eq"),[0x0a000029]=(typeof(Test),"Ne"),
   [0x060002f4]=(typeof(Widget),"Geometry"),[0x060002c2]=(typeof(Widget),"DrawCall"),
   [0x06000297]=(typeof(Draw),"Dirty"),[0x060046c3]=(typeof(Action),"Invoke")};
  var ops=typeof(OpCodes).GetFields().Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>(ushort)o.Value);
  byte[] code=(byte[])body.Clone();
  for(int i=0;i<code.Length;) {
   ushort op=code[i++];if(op==254)op=(ushort)(0xfe00+code[i++]);var kind=ops[op].OperandType;
   int n=kind switch{OperandType.InlineNone=>0,OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,OperandType.InlineVar=>2,OperandType.InlineI8 or OperandType.InlineR=>8,_=>4};
   if(kind==OperandType.InlineField){var (t,f)=fields[BitConverter.ToInt32(code,i)];BitConverter.GetBytes(il.GetTokenFor(t.GetField(f)!.FieldHandle)).CopyTo(code,i);}
   if(kind==OperandType.InlineMethod){var (t,m)=methods[BitConverter.ToInt32(code,i)];BitConverter.GetBytes(il.GetTokenFor(t.GetMethod(m)!.MethodHandle)).CopyTo(code,i);}
   i+=n;
  }
  il.SetCode(code,28);return method.CreateDelegate<Action<Panel>>();
 }
 static (string,int) Run(Action<Panel> fn,int seed) {
  var rng=new Random(seed);var owner=new Panel{id=1,stat=seed%2==0,until=2,now=1};var other=new Panel{id=2};
  var alias=new Panel{id=1};var dead=new Panel{id=3,dead=true};int callbacks=0;owner.changed=()=>callbacks++;
  Widget.list=new Items{data=Enumerable.Range(0,300).Select(i=>new Widget{panel=i%5==0?owner:i%5==1?other:i%5==2?alias:i%5==3?dead:null,lazyOwner=i%5==1?other:owner,enabled=rng.Next(4)!=0,updated=rng.Next(2)==0,draw=rng.Next(3)==0?null:new Draw()}).ToArray(),size=300};
  // A geometry callback can reparent a later widget; reads must stay live.
  Widget.list.data[0].onGeometry=()=>Widget.list.data[5].panel=other;
  Panel.full=seed%3==0;eqCalls=0;fn(owner);
  string state=string.Join(";",Widget.list.data.Select(w=>$"{w.enabledCalls}:{w.geometryCalls}:{ReferenceEquals(w.panel,owner)}:{w.draw?.dirty}"))+$"/{Panel.full}/{callbacks}";
  return (state,eqCalls+Widget.list.data.Sum(w=>w.ownerCalls));
 }
 static void Main(string[] args){
  using var pe=new PEReader(File.OpenRead(args[0]));var md=pe.GetMetadataReader();
  var def=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x439));var body=pe.GetMethodBody(def.RelativeVirtualAddress);
  var before=File.ReadAllBytes(args[1]);var after=File.ReadAllBytes(args[2]);
  Check(md.GetString(def.Name)=="UpdateWidgets" && body.GetILBytes()!.SequenceEqual(before) && before.Length==after.Length);
  Check(md.GetGuid(md.GetModuleDefinition().Mvid)==Guid.Parse("4900A8E6-EA01-4E8A-AE66-036836545DD2"));
  var a=(Action<Panel>)Make(before);var b=(Action<Panel>)Make(after);
  for(int seed=0;seed<100;seed++){var x=Run(a,seed);var y=Run(b,seed);Check(x.Item1==y.Item1 && y.Item2<x.Item2);}
  Console.WriteLine("PASS: original/patched panel IL, 30000 widget visits; enabled order, lazy/destroyed/alternate owners, live reparenting, geometry, dirty draws, callbacks and full rebuild; fewer owner/equality calls");
 }
}
