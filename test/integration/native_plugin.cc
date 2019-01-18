/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <gtest/gtest.h>

#include <string>

#include <ignition/plugin/Loader.hh>
#include <ignition/plugin/Register.hh>

class TestInterface
{
  public: virtual void input(const std::string &_input) = 0;
  public: virtual std::string output() const = 0;
};

class TestImplementation : public TestInterface
{
  public: void input(const std::string &_input) override
  {
    this->value = _input;
  }

  public: std::string output() const override
  {
    return this->value;
  }

  private: std::string value;
};

// This will be a native plugin of the test, available from any plugin loader
// instance.
IGNITION_ADD_PLUGIN(TestImplementation, TestInterface)

TEST(NativePlugin, Load)
{
  ignition::plugin::Loader loader;
  EXPECT_EQ(1u, loader.AllPlugins().size());

  ignition::plugin::PluginPtr plugin = loader.Instantiate("TestImplementation");
  EXPECT_FALSE(plugin.IsEmpty());

  TestInterface *test = plugin->QueryInterface<TestInterface>();
  ASSERT_NE(nullptr, test);

  test->input("some test string");
  EXPECT_EQ("some test string", test->output());

  ignition::plugin::PluginPtr copy = plugin;
  EXPECT_FALSE(copy.IsEmpty());

  plugin = ignition::plugin::PluginPtr();
  EXPECT_TRUE(plugin.IsEmpty());

  std::shared_ptr<TestInterface> shared =
      copy->QueryInterfaceSharedPtr<TestInterface>();
  ASSERT_NE(nullptr, shared);

  EXPECT_EQ("some test string", shared->output());

  copy = ignition::plugin::PluginPtr();
  EXPECT_TRUE(copy.IsEmpty());
}

/////////////////////////////////////////////////
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
