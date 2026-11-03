namespace UnityEngine {
 public class Object {
  public static bool operator !=(Object a,Object b){return !object.ReferenceEquals(a,b);}
  public static bool operator ==(Object a,Object b){return object.ReferenceEquals(a,b);}
  public override bool Equals(object x){return object.ReferenceEquals(this,x);}
  public override int GetHashCode(){return base.GetHashCode();}
 }
 public class GameObject:Object { public string name; public GameObject(string n){name=n;} }
 public struct Vector3 { public float x,y,z; public static Vector3 zero {get{return new Vector3();}} }
 public struct Quaternion { public float x,y,z,w; public static Quaternion Euler(Vector3 v){return new Quaternion{x=v.x,y=v.y,z=v.z,w=1};} }
}
