import{spawn}from'node:child_process';
export class Native {
  constructor(binary,config) {
    this.child=spawn(binary,[],{env:{...process.env,MODELD_BROWSER_CONFIG:config},stdio:['pipe','pipe','pipe']});
    this.next=0;this.pending=new Map();let input=Buffer.alloc(0);
    this.child.stdout.on('data',data=>{
      input=Buffer.concat([input,data]);
      while(input.length>=4&&input.length>=input.readUInt32LE(0)+4){
        const length=input.readUInt32LE(0),value=JSON.parse(input.subarray(4,length+4));input=input.subarray(length+4);
        const entry=this.pending.get(value.id);if(!entry)continue;
        if(!value.ok){this.pending.delete(value.id);entry.reject(Error(value.error?.message));continue;}
        if('data'in value){entry.parts.push(value.data);if(!value.done)continue;this.pending.delete(value.id);entry.resolve(Buffer.from(entry.parts.join(''),'base64'));}
        else{this.pending.delete(value.id);entry.resolve(value.result);}
      }
    });
    this.child.on('exit',()=>{for(const p of this.pending.values())p.reject(Error('Native host exited'));this.pending.clear();});
  }
  call(action,values={}) {
    const id=++this.next;
    return new Promise((resolve,reject)=>{
      this.pending.set(id,{resolve,reject,parts:[]});const value=Buffer.from(JSON.stringify({id,action,...values}));
      const header=Buffer.alloc(4);header.writeUInt32LE(value.length);this.child.stdin.write(Buffer.concat([header,value]));
    });
  }
  close(){this.child.stdin.end();}
}
