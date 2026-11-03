using System.Collections;
using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Xml;
using UnityEngine;

public class SpawnableParameter {
 public string ObjectName {get;set;} public uint ID {get;set;} public float RangeIn {get;set;}=20;
 public float RangeOut {get;set;}=30;public Vector3 Position {get;set;} public Quaternion Rotation {get;set;}
}
public class CustomSpawnable : SpawnableParameter { public float f;public int i;public uint u;public string s;public GameObject g; }
public class AnotherSpawnable : SpawnableParameter { public int i; }
public class ForeignParameter { public int i; }
public class ResourceManager { public GameObject GetSpawnableGameObject(string n){return n=="missing"?null:new GameObject(n);} }
public class BlockSpawnableParameterContainer {
 public int Block,Layer;public List<SpawnableParameter> Items=new();
 public BlockSpawnableParameterContainer(int b,int l){Block=b;Layer=l;}
 public void AddParameter(SpawnableParameter p){Items.Add(p);}
}
public class StageSpawnableParameterContainer {
 public List<BlockSpawnableParameterContainer> Blocks=new(); public List<int> PublishedCounts=new();
 public void AddData(int b,int l,BlockSpawnableParameterContainer x){if(x.Block!=b||x.Layer!=l)throw new Exception();Blocks.Add(x);PublishedCounts.Add(x.Items.Count);}
}
public class SpawnableParser {
 public delegate void ObjectsFn(SpawnableParser p,ResourceManager r,XmlReader x,BlockSpawnableParameterContainer b);
 public delegate Vector3 VectorFn(XmlReader r);
 public delegate void UserFn(ResourceManager r,XmlReader x,object o,Type t,string n);
 public static int readInvocations;
 public static ObjectsFn objects;public static VectorFn vector;public static UserFn user;
 public static Dictionary<string,int> map4,map5,map6,map7,map8;
 public void ReadObjects(ResourceManager r,XmlReader x,BlockSpawnableParameterContainer b){readInvocations++;objects(this,r,x,b);}
 private static Vector3 ReadVector3(XmlReader r){return vector(r);}
 private static void ReadUserParameters(ResourceManager r,XmlReader x,object o,Type t,string n){user(r,x,o,t,n);}
}
public class OriginalLayer : IEnumerator,IDisposable {
 public delegate bool Step(OriginalLayer s);public static Step step;
 public XmlReader reader;public int layerID,blockID;public BlockSpawnableParameterContainer block;
 public ResourceManager manager;public StageSpawnableParameterContainer data;public int pc;public object current;public SpawnableParser parser;
 public object Current=>current;public bool MoveNext()=>step(this);public void Dispose(){pc=-1;}public void Reset(){throw new NotSupportedException();}
}
static class Test {
 static void Check(bool b){if(!b)throw new Exception("Stage parser regression");}
 static MethodInfo M(Type t,string n,params Type[] p)=>t.GetMethod(n,p)!;
 static object Resolve(int token) {
  return token switch {
   0x01000041=>typeof(float),0x01000007=>typeof(int),0x01000084=>typeof(uint),
   0x020008cf=>typeof(SpawnableParameter),
   0x0a00000c=>M(typeof(string),"IsNullOrEmpty",typeof(string)),
   0x0a0000aa=>typeof(Dictionary<string,int>).GetConstructor(new[]{typeof(int)})!,
   0x0a0000ab=>M(typeof(Dictionary<string,int>),"Add",typeof(string),typeof(int)),
   0x0a0000ac=>M(typeof(Dictionary<string,int>),"TryGetValue",typeof(string),typeof(int).MakeByRefType()),
   0x0a00126c=>M(typeof(XmlReader),"MoveToFirstAttribute"),
   0x0a00126d=>typeof(XmlReader).GetProperty("Name")!.GetMethod!,
   0x0a00126e=>typeof(XmlReader).GetProperty("Value")!.GetMethod!,
   0x0a00126f=>M(typeof(XmlReader),"MoveToNextAttribute"),
   0x0a001270=>M(typeof(XmlReader),"MoveToElement"),
   0x0a001271=>M(typeof(XmlReader),"ReadToDescendant",typeof(string)),
   0x0a001272=>M(typeof(Type),"GetField",typeof(string),typeof(BindingFlags)),
   0x0a001273=>M(typeof(uint),"TryParse",typeof(string),typeof(uint).MakeByRefType()),
   0x0a001274=>M(typeof(uint),"Parse",typeof(string),typeof(NumberStyles)),
   0x0a001275=>M(typeof(XmlReader),"ReadToNextSibling",typeof(string)),
   0x0a001276=>M(typeof(Type),"GetType",typeof(string)),
   0x0a001277=>M(typeof(XmlReader),"ReadSubtree"),
   0x0a001278=>M(typeof(XmlReader),"Skip"),
   0x0a001279=>M(typeof(XmlReader),"Read"),
   0x0a00127a=>M(typeof(XmlReader),"Close"),
   0x0a00127d=>M(typeof(XmlReader),"MoveToAttribute",typeof(string)),
   0x0a00068e=>M(typeof(int),"Parse",typeof(string)),
   0x0a00068f=>M(typeof(float),"Parse",typeof(string)),
   0x0a000b97=>M(typeof(Activator),"CreateInstance",typeof(Type)),
   0x0a000b9f=>M(typeof(FieldInfo),"SetValue",typeof(object),typeof(object)),
   0x0a0005bc=>M(typeof(float),"TryParse",typeof(string),typeof(float).MakeByRefType()),
   0x0a0003c8=>M(typeof(int),"TryParse",typeof(string),typeof(int).MakeByRefType()),
   0x0a000029=>M(typeof(UnityEngine.Object),"op_Inequality",typeof(UnityEngine.Object),typeof(UnityEngine.Object)),
   0x0a00005b=>typeof(Vector3).GetProperty("zero")!.GetMethod!,
   0x0a000038=>typeof(Vector3).GetField("x")!,0x0a00003b=>typeof(Vector3).GetField("y")!,0x0a000079=>typeof(Vector3).GetField("z")!,
   0x0a0000b5=>M(typeof(Quaternion),"Euler",typeof(Vector3)),
   0x0600308a=>M(typeof(ResourceManager),"GetSpawnableGameObject",typeof(string)),
   0x06003c05=>typeof(SpawnableParameter).GetConstructor(Type.EmptyTypes)!,
   0x06003c08=>typeof(SpawnableParameter).GetProperty("ID")!.SetMethod!,
   0x06003c0a=>typeof(SpawnableParameter).GetProperty("ObjectName")!.SetMethod!,
   0x06003c0c=>typeof(SpawnableParameter).GetProperty("Position")!.SetMethod!,
   0x06003c0e=>typeof(SpawnableParameter).GetProperty("Rotation")!.SetMethod!,
   0x06003c10=>typeof(SpawnableParameter).GetProperty("RangeIn")!.SetMethod!,
   0x06003c12=>typeof(SpawnableParameter).GetProperty("RangeOut")!.SetMethod!,
   0x06003c16=>typeof(SpawnableParser).GetMethod("ReadVector3",BindingFlags.Static|BindingFlags.NonPublic)!,
   0x06003c17=>typeof(SpawnableParser).GetMethod("ReadUserParameters",BindingFlags.Static|BindingFlags.NonPublic)!,
   0x06003c18=>M(typeof(SpawnableParser),"ReadObjects",typeof(ResourceManager),typeof(XmlReader),typeof(BlockSpawnableParameterContainer)),
   0x06000773=>typeof(BlockSpawnableParameterContainer).GetConstructor(new[]{typeof(int),typeof(int)})!,
   0x06000777=>M(typeof(BlockSpawnableParameterContainer),"AddParameter",typeof(SpawnableParameter)),
   0x06003e70=>M(typeof(StageSpawnableParameterContainer),"AddData",typeof(int),typeof(int),typeof(BlockSpawnableParameterContainer)),
   >=0x04003222 and <=0x04003226=>typeof(SpawnableParser).GetField("map"+(token-0x0400321e))!,
   0x040043d2=>typeof(OriginalLayer).GetField("reader")!,0x040043d3=>typeof(OriginalLayer).GetField("layerID")!,
   0x040043d4=>typeof(OriginalLayer).GetField("blockID")!,0x040043d5=>typeof(OriginalLayer).GetField("block")!,
   0x040043d6=>typeof(OriginalLayer).GetField("manager")!,0x040043d7=>typeof(OriginalLayer).GetField("data")!,
   0x040043d8=>typeof(OriginalLayer).GetField("pc")!,0x040043d9=>typeof(OriginalLayer).GetField("current")!,
   0x040043de=>typeof(OriginalLayer).GetField("parser")!,
   _=>throw new Exception("Unmapped original token "+token.ToString("x8"))
  };
 }
 record Instruction(int At,OpCode Op,object Arg);
 static Delegate Clone(PEReader pe,MetadataReader md,int rid,Type result,Type[] args,Type[] locals,Type signature,byte[] replacement=null) {
  var m=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(rid));var body=pe.GetMethodBody(m.RelativeVirtualAddress);
  Check(body.ExceptionRegions.Length==0);byte[] bytes=replacement ?? body.GetILBytes()!;
  var ops=typeof(OpCodes).GetFields().Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>(ushort)o.Value);
  List<Instruction> code=new();
  for(int i=0;i<bytes.Length;){int at=i;ushort v=bytes[i++];if(v==0xfe)v=(ushort)(0xfe00|bytes[i++]);OpCode op=ops[v];object arg=null;
   switch(op.OperandType){
    case OperandType.InlineNone:break;
    case OperandType.ShortInlineI:arg=(sbyte)bytes[i++];break;
    case OperandType.ShortInlineVar:arg=bytes[i++];break;
    case OperandType.InlineI:arg=BitConverter.ToInt32(bytes,i);i+=4;break;
    case OperandType.ShortInlineR:arg=BitConverter.ToSingle(bytes,i);i+=4;break;
    case OperandType.InlineString:arg=md.GetUserString(MetadataTokens.UserStringHandle(BitConverter.ToInt32(bytes,i)&0xffffff));i+=4;break;
    case OperandType.InlineType:case OperandType.InlineField:case OperandType.InlineMethod:arg=Resolve(BitConverter.ToInt32(bytes,i));i+=4;break;
    case OperandType.ShortInlineBrTarget:arg=i+1+(sbyte)bytes[i];i++;break;
    case OperandType.InlineBrTarget:arg=i+4+BitConverter.ToInt32(bytes,i);i+=4;break;
    case OperandType.InlineSwitch:int count=BitConverter.ToInt32(bytes,i);i+=4;int b=i+4*count;int[] targets=new int[count];for(int n=0;n<count;n++){targets[n]=b+BitConverter.ToInt32(bytes,i);i+=4;}arg=targets;break;
    default:throw new Exception(op.Name+" "+op.OperandType);
   }
   code.Add(new Instruction(at,op,arg));
  }
  var dynamic=new DynamicMethod("APK_"+rid,result,args,typeof(SpawnableParser),true);var il=dynamic.GetILGenerator();
  foreach(var t in locals)il.DeclareLocal(t);
  var labels=code.ToDictionary(x=>x.At,x=>il.DefineLabel());labels[bytes.Length]=il.DefineLabel();
  foreach(var x in code){il.MarkLabel(labels[x.At]);switch(x.Arg){
   case null:il.Emit(x.Op);break;
   case int n:if(x.Op.OperandType==OperandType.InlineBrTarget||x.Op.OperandType==OperandType.ShortInlineBrTarget)il.Emit(x.Op,labels[n]);else il.Emit(x.Op,n);break;
   case int[] targets:il.Emit(x.Op,targets.Select(n=>labels[n]).ToArray());break;
   case byte n:il.Emit(x.Op,n);break;case sbyte n:il.Emit(x.Op,n);break;case float f:il.Emit(x.Op,f);break;
   case string s:il.Emit(x.Op,s);break;case Type t:il.Emit(x.Op,t);break;
   case MethodInfo f:il.Emit(x.Op,f);break;case ConstructorInfo c:il.Emit(x.Op,c);break;case FieldInfo f:il.Emit(x.Op,f);break;
   default:throw new Exception();
  }}
  il.MarkLabel(labels[bytes.Length]);return dynamic.CreateDelegate(signature);
 }
 static Type S=typeof(string),I=typeof(int),U=typeof(uint),D=typeof(Dictionary<string,int>),R=typeof(XmlReader),P=typeof(SpawnableParameter),T=typeof(Type),O=typeof(object),V=typeof(Vector3),MGR=typeof(ResourceManager),B=typeof(BlockSpawnableParameterContainer);
 static SpawnableParser.ObjectsFn originalObjects,batchedObjects;
 static OriginalLayer.Step originalLayer,batchedLayer;
 static void LoadOriginal(PEReader pe,MetadataReader md,byte[] layerCode){
  SpawnableParser.vector=(SpawnableParser.VectorFn)Clone(pe,md,0x3c16,V,new[]{R},new[]{V,S,D,I},typeof(SpawnableParser.VectorFn));
  SpawnableParser.user=(SpawnableParser.UserFn)Clone(pe,md,0x3c17,typeof(void),new[]{MGR,R,O,T,S},new[]{P,S,S,S,typeof(FieldInfo),typeof(GameObject),typeof(float),I,U,S,S,D,I},typeof(SpawnableParser.UserFn));
  SpawnableParser.objects=(SpawnableParser.ObjectsFn)Clone(pe,md,0x3c18,typeof(void),new[]{typeof(SpawnableParser),MGR,R,B},new[]{S,U,S,I,I,T,O,P,R,V,S,D,I},typeof(SpawnableParser.ObjectsFn));
  originalObjects=SpawnableParser.objects;
  OriginalLayer.step=(OriginalLayer.Step)Clone(pe,md,0x4c80,typeof(bool),new[]{typeof(OriginalLayer)},new[]{U,typeof(bool)},typeof(OriginalLayer.Step));
  originalLayer=OriginalLayer.step;
  byte[] objectCode=pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x3c18)).RelativeVirtualAddress).GetILBytes()!;
  Array.Clear(objectCode,0x298,16);
  batchedObjects=(SpawnableParser.ObjectsFn)Clone(pe,md,0x3c18,typeof(void),new[]{typeof(SpawnableParser),MGR,R,B},new[]{S,U,S,I,I,T,O,P,R,V,S,D,I},typeof(SpawnableParser.ObjectsFn),objectCode);
  batchedLayer=(OriginalLayer.Step)Clone(pe,md,0x4c80,typeof(bool),new[]{typeof(OriginalLayer)},new[]{U,typeof(bool)},typeof(OriginalLayer.Step),layerCode);
 }
 static string Dump(StageSpawnableParameterContainer data){
  return string.Join("|",data.Blocks.Select(b=>$"{b.Block}:{b.Layer}:"+string.Join(";",b.Items.Select(p=>{
   var v=p.Position;var a=p.Rotation;
   return $"{p.GetType().FullName},{p.ObjectName},{p.ID},{p.RangeIn},{p.RangeOut},{v.x},{v.y},{v.z},{a.x},{a.y},{a.z},{a.w},"+
    string.Join(",",p.GetType().GetFields().Select(f=>f.Name+"="+(f.GetValue(p) is GameObject g?g.name:f.GetValue(p))));
  }))));
 }
 static (string,string,int) Run(string xml,bool batched,int stopAfter=-1){
  var data=new StageSpawnableParameterContainer();var reader=XmlReader.Create(new StringReader(xml));reader.ReadToFollowing("Layer");
  var parser=new SpawnableParser();var manager=new ResourceManager();
  SpawnableParser.objects=batched?batchedObjects:originalObjects;OriginalLayer.step=batched?batchedLayer:originalLayer;
  IEnumerator iterator=new OriginalLayer{reader=reader,blockID=7,manager=manager,data=data,parser=parser};
  string error=null;int ticks=0,prev=0;
  try {while(true){
   int before=SpawnableParser.readInvocations;
   bool more=iterator.MoveNext();
   if(batched)Check(SpawnableParser.readInvocations-before<=16);
   for(int j=0;j<data.Blocks.Count;j++)Check(data.Blocks[j].Items.Count==data.PublishedCounts[j]);
   if(!more){Check(!iterator.MoveNext());break;}
   Check(iterator.Current==null);ticks++;Check(ticks<5000);
   int items=data.Blocks.Sum(b=>b.Items.Count);
   // No partially built layer can be published, regardless of batch boundaries.
   Check(items>=prev);prev=items;
   GC.Collect(); // State/reader/custom objects must remain rooted across yields.
   if(stopAfter==ticks){((IDisposable)iterator).Dispose();Check(!iterator.MoveNext());break;}
  }}catch(Exception e){error=e.GetType().FullName;Check(!iterator.MoveNext());}
  string dump=Dump(data);reader.Close();return(dump,error,ticks);
 }
 static void Main(string[] args){
  using var pe=new PEReader(File.OpenRead(args[0]));var md=pe.GetMetadataReader();
  Check(md.GetGuid(md.GetModuleDefinition().Mvid)==Guid.Parse("4900a8e6-ea01-4e8a-ae66-036836545dd2"));
  if(args.Length==3 && args[1]=="--export") {
   Directory.CreateDirectory(args[2]);
   foreach(var pair in new[]{(0x3c18,"ReadObjects.il"),(0x4c80,"ReadLayer.il")})File.WriteAllBytes(Path.Combine(args[2],pair.Item2),pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(pair.Item1)).RelativeVirtualAddress).GetILBytes()!);
   return;
  }
  Check(pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x3c18)).RelativeVirtualAddress).GetILBytes()!.SequenceEqual(File.ReadAllBytes(args[2])));
  Check(pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x4c80)).RelativeVirtualAddress).GetILBytes()!.SequenceEqual(File.ReadAllBytes(args[3])));
  LoadOriginal(pe,md,File.ReadAllBytes(args[1]));
  foreach(var pair in new[]{(0x3c18,"ReadObjects","SpawnableParser"),(0x4c80,"MoveNext","<ReadLayer>c__IteratorC4")}) {
   var m=md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(pair.Item1));
   Check(md.GetString(m.Name)==pair.Item2 && md.GetString(md.GetTypeDefinition(m.GetDeclaringType()).Name)==pair.Item3);
  }
  Check(Convert.ToHexString(md.GetBlobBytes(md.GetStandaloneSignature(pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x4c80)).RelativeVirtualAddress).LocalSignature).Signature))=="07020902");
  // The helper is private and has exactly this one IL caller; pairing the
  // edits cannot truncate another gameplay call to ReadObjects.
  var opTable=typeof(OpCodes).GetFields().Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>(ushort)o.Value);
  List<int> callers=new();
  foreach(var h in md.MethodDefinitions){var m=md.GetMethodDefinition(h);if(m.RelativeVirtualAddress==0)continue;var code=pe.GetMethodBody(m.RelativeVirtualAddress).GetILBytes()!;
   for(int i=0;i<code.Length;){ushort v=code[i++];if(v==0xfe)v=(ushort)(0xfe00|code[i++]);var op=opTable[v];
    int n=op.OperandType switch {OperandType.InlineNone=>0,OperandType.ShortInlineI or OperandType.ShortInlineVar or OperandType.ShortInlineBrTarget=>1,OperandType.InlineVar=>2,OperandType.InlineI8 or OperandType.InlineR=>8,OperandType.InlineSwitch=>4+4*BitConverter.ToInt32(code,i),_=>4};
    if(op.OperandType==OperandType.InlineMethod && BitConverter.ToInt32(code,i)==0x06003c18)callers.Add(MetadataTokens.GetToken(h));i+=n;
   }
  }
  Check(callers.SequenceEqual(new[]{0x06004c80}));
  Check(Convert.ToHexString(pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x4c7d)).RelativeVirtualAddress).GetILBytes()!)=="02280900000A2A");
  Check(Convert.ToHexString(pe.GetMethodBody(md.GetMethodDefinition(MetadataTokens.MethodDefinitionHandle(0x4c7f)).RelativeVirtualAddress).GetILBytes()!)=="027BD94300042A");
  List<string> cases=new();
  foreach(int n in new[]{0,1,15,16,17,31,32,33,128,513}){
   var objects=string.Join("",Enumerable.Range(0,n).Select(i=>$"<Obj C='ring{i}' ID='{i:X}' I='{i%45}' O='{i%80}'><P X='{i}.5' Y='-2' Z='3'/><A X='0' Y='0' Z='90'/></Obj>"));
   cases.Add("<Root><Layer ID='1'>"+objects+"</Layer><Layer ID='2'/><Layer ID='3'>"+objects+"</Layer></Root>");
  }
  cases.Add("<Root><Layer ID='1'><Obj C='a' ID='A' I='71' O='82'/><Obj/><Obj C='b'/><Obj ID='FFFFFFFF'/><Obj P='CustomSpawnable'><PS><It N='i' T='i' V='42'/><It N='f' T='f' V='1.5'/><It N='u' T='u' V='12'/><It N='s' T='s' V='text'/><It N='g' T='g' V='prefab'/></PS></Obj><Obj/><Obj P='missing-type'/><Obj/></Layer></Root>");
  cases.Add("<Root><Layer/><Layer ID='9'><Obj ID='A'/><Obj C='x'/><Obj P='ForeignParameter'/><Obj P='AnotherSpawnable'><PS><It N='i' T='i' V='bad'/><It N='absent' T='s' V='x'/></PS></Obj></Layer></Root>");
  cases.Add("<Root><Layer ID='-2'><Obj C='x'><P X='1' Q='9' Z='3'></P><A Y='5'/></Obj></Layer></Root>");
  foreach(string bad in new[]{"ID='nothex'","I='oops'","O='999999999999999'","P='System.String'"})cases.Add("<Root><Layer ID='1'><Obj C='x' "+bad+"/></Layer></Root>");
  cases.Add("<Root><Layer ID='bad'><Obj C='x'/></Layer></Root>");
  var rng=new Random(70);
  for(int c=0;c<80;c++){
   string xml="<Root>";
   for(int l=0;l<rng.Next(1,5);l++) {
    xml+="<Layer ID='"+l+"'>";
    for(int n=0;n<rng.Next(1,100);n++) {
     xml+="<Obj";
     if(rng.Next(3)>0)xml+=" C='r"+n+"'";
     if(rng.Next(3)>0)xml+=" ID='"+rng.Next(100000).ToString("X")+"'";
     if(rng.Next(3)>0)xml+=" I='"+rng.Next(100)+"'";
     if(rng.Next(3)>0)xml+=" O='"+rng.Next(100)+"'";
     if(rng.Next(4)==0)xml+=" P='CustomSpawnable'";
     xml+="><P X='1.5' Y='-5' Z='0'/><PS><It N='i' T='i' V='27'/></PS></Obj>";
    }
    xml+="</Layer>";
   }
   cases.Add(xml+"</Root>");
  }
  int runs=0;
  foreach(var culture in new[]{"en-US","pt-BR"}){CultureInfo.CurrentCulture=CultureInfo.GetCultureInfo(culture);
   foreach(string xml in cases){var a=Run(xml,false);var b=Run(xml,true);if(a.Item1!=b.Item1||a.Item2!=b.Item2){Console.WriteLine("FAILED "+culture+" "+xml);Console.WriteLine("BEFORE "+a);Console.WriteLine("AFTER "+b);}Check(a.Item1==b.Item1&&a.Item2==b.Item2);runs++;}
  }
  foreach(int n in new[]{1,16,17,32,33,128,513}){
   string xml="<Root><Layer ID='1'>"+string.Concat(Enumerable.Repeat("<Obj C='a'/>",n))+"</Layer></Root>";
   var a=Run(xml,false);var b=Run(xml,true);Check(a.Item3==1&&b.Item3==(n+15)/16&&a.Item1==b.Item1);runs++;
   var disposed=Run(xml,true,1);if(n>16)Check(disposed.Item1=="");
  }
  Console.WriteLine($"PASS: {runs} actual-APK IL vs batched parser cases; omitted attributes/defaults, vectors/custom fields/types, two cultures, exceptions, whole-layer publication, GC across yields, batch boundaries and disposal");
 }
}
