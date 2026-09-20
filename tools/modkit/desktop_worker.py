"""Private JSON-lines worker owned by Electron. No listening sockets."""
import json,sys,traceback
from pathlib import Path
# Isolated bundled Python does not implicitly add the entry script directory.
sys.path.insert(0,str(Path(__file__).resolve().parent))
from service import AssetFile,CACHE,runtime_registry,read_resource,mutate

def main():
 CACHE.mkdir(parents=True,exist_ok=True);runtime_registry()
 for line in sys.stdin:
  request={}
  try:
   request=json.loads(line)
   if request['op']=='read':result=read_resource(request['path'])
   elif request['op']=='mutate':result=mutate(request['path'],request['data'])
   elif request['op']=='ping':result={'ready':True}
   else:raise ValueError('Unknown worker operation')
   if isinstance(result,AssetFile):result={'assetFile':str(result.path),'download':result.download}
   response={'id':request['id'],'result':result}
  except Exception as e:
   traceback.print_exc(file=sys.stderr);response={'id':request.get('id'),'error':str(e)}
  print(json.dumps(response,allow_nan=False,separators=(',',':')),flush=True)
if __name__=='__main__':main()
