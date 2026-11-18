import fs from 'node:fs';
export class Cdp {
  constructor(url) {
    this.socket=new WebSocket(url);this.pending=new Map();this.listeners=new Set();this.next=0;
    this.ready=new Promise((resolve,reject)=>{this.socket.addEventListener('open',resolve,{once:true});this.socket.addEventListener('error',()=>reject(Error('Browser debugging connection failed')),{once:true});});
    this.socket.addEventListener('message',event=>{
      const message=JSON.parse(event.data);
      if(message.id&&this.pending.has(message.id)){const entry=this.pending.get(message.id);this.pending.delete(message.id);clearTimeout(entry.timer);message.error?entry.reject(Error(JSON.stringify(message.error))):entry.resolve(message.result);}
      else for(const listener of this.listeners)listener(message);
    });
    this.socket.addEventListener('close',()=>{for(const entry of this.pending.values()){clearTimeout(entry.timer);entry.reject(Error('Browser debugging connection closed'));}this.pending.clear();});
  }
  static async profile(directory) {
    const [port,path]=fs.readFileSync(directory+'/DevToolsActivePort','utf8').trim().split('\n');
    const cdp=new Cdp('ws://127.0.0.1:'+port+path);await cdp.ready;return cdp;
  }
  async send(method,params={},sessionId) {
    await this.ready;const id=++this.next;
    return new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>{this.pending.delete(id);reject(Error('Browser command timed out: '+method));},30000);
      this.pending.set(id,{resolve,reject,timer});this.socket.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));
    });
  }
  async attach(targetId){return (await this.send('Target.attachToTarget',{targetId,flatten:true})).sessionId;}
  async evaluate(session,expression){const value=await this.send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},session);if(value.exceptionDetails)throw Error(value.exceptionDetails.text+': '+(value.exceptionDetails.exception?.description||''));return value.result?.value;}
  close(){this.socket.close();}
}
