"""Failed preparation must retain valid state and never queue the wrong room."""
import json, os, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
from project_config import ROOT
from prepare_content_runtime import prepare

class ContentPlanTests(unittest.TestCase):
    def setUp(self):
        root=ROOT/"build/tests";root.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(prefix="content-plan-",dir=root)
        self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.output=self.root/"plan.env"
        self.env=patch.dict(os.environ,{"MELEE_CONTENT_STATE":str(self.root/"state.json"),"MELEE_CONTENT_TEST_PLAN":"","MELEE_CONTENT_TEST_MOD_REGISTRY":"","MELEE_CONTENT_JOIN_ROOM":"123","MELEE_CONTENT_RETURN":"1","MELEE_CONTENT_RESTART":str(self.root/"request.json")})
        self.env.start();self.addCleanup(self.env.stop)
        (self.root/"state.json").write_text(json.dumps({"schema":1,"akaneia":None}))

    def test_manager_return_clears_previous_join_target(self):
        (self.root/"request.json").write_text(json.dumps({"schema":1,"room":0}))
        values=prepare(self.output,resume=True)
        self.assertEqual(values["MELEE_CONTENT_JOIN_ROOM"],"")
        self.assertEqual(values["MELEE_CONTENT_RETURN"],"1")

    def test_rejoin_preserves_exact_room_and_player_name(self):
        (self.root/"request.json").write_text(json.dumps({"schema":1,"room":47,"server":"localhost:1234","name":"Test Player"}))
        values=prepare(self.output,resume=True)
        self.assertEqual(values["MELEE_CONTENT_JOIN_ROOM"],"47")
        self.assertEqual(values["MELEE_NETPLAY_NAME"],"Test Player")

    def test_invalid_input_does_not_replace_previous_plan(self):
        self.output.write_bytes(b"last verified plan")
        with patch.dict(os.environ,{"MELEE_CONTENT_JOIN_ROOM":"123\ninjected"}):
            with self.assertRaisesRegex(ValueError,"Invalid boot input"):prepare(self.output)
        self.assertEqual(self.output.read_bytes(),b"last verified plan")

    def test_recovery_restores_previous_enabled_state_and_cancels_join(self):
        (self.root/"state.json").write_text(json.dumps({"schema":1,"akaneia":{"enabled":True,"directory":"retained-install","isoSHA256":"retained-hash"}}))
        with patch.dict(os.environ,{"MELEE_CONTENT_ENABLED":"0"}):
            values=prepare(self.output,recover=True)
        record=json.loads((self.root/"state.json").read_text())["akaneia"]
        self.assertFalse(record["enabled"])
        self.assertEqual(record["directory"],"retained-install")
        self.assertEqual(values["MELEE_CONTENT_ENABLED"],"0")
        self.assertEqual(values["MELEE_CONTENT_JOIN_ROOM"],"")

if __name__=="__main__":unittest.main()
